# Embedded Linux Projekt: System Monitor

## Architektur

### Übersicht der Architektur
![Architektur](https://github.com/user-attachments/assets/f4b9a00d-c1cc-47a1-89ff-5391cb528a4b)

### sysmond – Userspace Programm für Messungen
Für jede Messung die vorgenommen werden soll, wird mittels `fork()` vom Elternprozess ein Kindprozess abgespalten. Der Elternprozess übernimmt dabei die Aufgabe der Kommunikation mit dem Kernelmodul und sorgt für die Aggregation der Daten, während die Kindprozesse die Messungen ausführen und die ermittelten Werte an den Elternprozess zurück übermitteln.

Zwischen einem Kind und dem Elternprozess werden jeweils 2 Pipes erzeugt. Diese dienen dazu eine bidirektionale Kommunikation zwischen Eltern- und Kindprozess zu ermöglichen.

Wir haben uns für forking im Zusammenspiel mit anonymen Pipes entschieden, da forking und piping uns noch aus dem Modul verteilte Systeme geläufig waren und sich anonyme Pipes hervorragend dafür eignen um zwischen einem Eltern- und dessen Kindprozessen zu kommunizieren.  

Eine der Pipes dient dazu ein Signal vom Elternprozess an die Kindprozesse zu übermitteln, um diesen zu signalisieren, dass sie ihre ermittelten Messwerte an den Elternprozess übertragen können, und der vorher übertragene Wert bereits erfolgreich gelesen wurde. Das Signal dient dazu die Anstauung von Messwerten im Buffer der Pipes zu verhindern.
Das Auslesen der Messdaten in den Kindprozessen erfolgt dabei bereits vor der Übermittlung des Signals, jedoch warten die Kindprozesse vor dem Zurücksenden der Daten bis sie die Aufforderung vom Elternprozess erhalten. Durch diese Behandlung der Signale kann es vorkommen, dass die Messdaten nicht mehr ganz aktuell sind, wenn sie übertragen werden. Jedoch sorgt es auch dafür, dass die Zeit außerhalb des Übertragungsslots bereits sinnvoll für die Sammlung der Messwerte genutzt werden kann.

Die andere Pipe dient dementsprechend dazu, auf Anfrage vom Elternprozess die ermittelten Messwerte an diesen zu übertragen.

Die Kontrolle des Timings und der Einhaltung der Zeitslots erfolgt ebenfalls komplett im Userspace. Dafür ermitteln wir vor dem Beginn einer Datenermittlung und Übertragung über die GPIO-Pins einen Zeitstempel mit der aktuellen Systemzeit, und addieren zu diesem Zeitstempel die Pause die benötigt wird, um allen anderen Sendern genügend Zeit zu geben, ihre Daten zu übermitteln.

Diese Pause wird dabei wie folgt berechnet.

```
NUM_JOBS * TIME_PER_JOB * NUM_SENDERS
```

Hat man beispielsweise 3 Messwerte die Ausgelesen werden, eine maximale Zeit von 25 Millisekunde zum Auslesen des jeweiligen Werts und insgesamt 4 Sender im gesamten System ergibt sich eine Zeit von `3 ∗ 25ms ∗ 4 = 300ms` für einen Übertragungsdurchlauf des gesamten Systems. Diese Zeit entspricht der nötigen Pause.

Nach abgeschlossener Übertragung wird der Elternprozess mittels `clock_nanosleep()` schlafen gelegt, bis die benötigte Zeit verstrichen ist.

### system_monitor – Kernel Modul
Das Kernelmodul stellt unter `/proc/system-monitor` eine Pseudodatei zur Verfügung, über welche mithilfe von Schreiboperationen auf diese Datei Daten an das Modul übermittelt werden können. Für diese Daten wird innerhalb des Kernel Moduls eine `CRC-32/JAMCRC` Prüfsumme generiert, anschließend an die Daten angefügt und über das im Folgenden beschriebene GPIO-Protokoll übertragen.

## GPIO-Protokoll
Unser GPIO-Protokoll verwendet zwei Leitungen. Eine Datenleitung (MSD) und eine Clockleitung (MSC). Während MSC `high` ist, liegt an MSD ein neues valides Datenbit an und kann ausgelesen werden. Bevor das nächste Datenbit auf MSD gelegt wird, wechselt MSC auf `low` um mit dem nächsten wechsel auf `high` den Beginn des nächsten Datenbits zu signalisieren.

Die gelesenen Werte auf MSD sind wie folgt zu interpretieren:
- `MSD high wenn MSC high → 0b1 wird übertragen`
- `MSD low wenn MSC high → 0b0 wird übertragen`

Die einzelnen Bits in einem Byte werden in LSB-first Reihenfolge übertragen, und alle Multi-Byte-Werte werden in Little Endian Reihenfolge übertragen.

Ein Datenframe welcher über MSD übermittelt wird, ist wie folgt aufgebaut:
```
+----------+---------+---------------+----------+------------------+
| SenderID | WertID0 |     Wert0     |   ...    |   CRC-32/JAMCRC  |
+----------+---------+---------------+----------+------------------+
| 1 Byte   | 1 Byte  | 2 Byte        | N*3 Byte |  4 Byte          |
|          |         | Little Endian |          |  Little Endian   |
+----------+---------+---------------+----------+------------------+
```

Für die Werte-IDs verwenden wir einen nicht vorzeichenbehafteten 8-bit Integer, mit diesem lassen sich 256 Werte (Werte-ID 0 - 255) darstellen. Somit ist es unserem System tendenziell möglich 256 verschiedene Werte zu überwachen. 
Die Wahl der Länge von einem Byte wurde von uns getroffen, da wir 256 mögliche überwachbare Werte für unser System als ausreichend befunden haben.

Die Werte selbst werden als nicht vorzeichenbehafteten 16-bit Integer übertragen.
Wir haben uns hier für 16-bit entschieden da man mit 16-bit bereits deutlich größere und für unser Projekt volkommen ausreichende Zahlen darstellen könnte. 8 Bit haben hier nicht ausgereicht, da einer der Werte der von uns überwacht wird (CPU Temperatur in Milligrad Celsius) größer ist als 255, also mit 8 Bit nicht dargestellt werden kann.

Für die Realisierung über GPIOs mit bitbashing haben wir uns entschieden, da wir uns zunächst mit einem I²C-Ansatz beschäftigt haben, diesen nach längerer Zeit aber verwerfen mussten, da wir Probleme mit dem Device Tree hatten und es uns nicht möglich war das Interface zu aktivieren. Ebenso ist es vorteilhaft, dass wir für unseren jetzigen Ansatz keinen Empfänger simulieren müssen um die Datenframes zu versenden.

### Timing
![Timing](https://github.com/user-attachments/assets/e4879aa5-87c6-4aef-a7eb-ba1d2a7bb23c)

## Automatischer Start der Überwachung und Übertragung bei Systemstart
Um die automatische Überwachung bei Systemstart zu realisieren haben wir das System so eingestellt, dass das Kernel Modul beim Boot automatisch geladen wird und somit zur Kommunikation mit dem Datenbus zur Verfügung steht. Dafür wurde das Modul File `system_monitor.ko` in den entsprechenden Ordner unter `/lib/modules/$(uname -r)/` kopiert, und ein entsprechender Eintrag `monitoring_system` in der Datei `/etc/modules` angelegt. Um dem System das neu eingefügte Modul bekannt zu machen muss mit `sudo depmod` noch der Modulcache aktualisiert werden.

Ebenso haben wir einen systemd-service erstellt welcher nach dem Systemstart dafür sorgt, dass unser Userspace Programm `sysmond` automatisch gestartet wird und mit der Überwachung beginnt. Dafür wurde das entsprechende File unter `/etc/systemd/system/sysmond.service` erstellt und gefüllt:

```ini
[UNIT]
Description=System Monitor Service
After=sysinit.target systemd-udev-settle.service
StartLimitIntervalSec=0

[Service]
Type=simple
Restart=always
RestartSec=1
ExecStart=/home/user/embedded-linux-project/sysmond

[Install]
WantedBy=multi-user.target
```

Anschließend wird der Service mit `sudo systemctl enable --now sysmond` gestartet und der automatische Start beim Bootvorgang aktiviert.

## Sicherstellung der Echtzeitfähigkeit
Um die Echtzeitfähigkeit zu gewährleisten, haben wir den `6.1.0-31-rt-arm64` Kernel mit Realtime Funktionalität installiert. Ebenso setzen wir in unserem `sysmond` Prozess und seinen Kindern die entsprechenden Parameter, um die Prozesse mit Realtime Priorität zu behaften. Dabei setzen wir die Priorität des Elternprozesses auf 99 (max. Wert) um sicherzustellen, dass dieser immer Laufen darf wenn er bereit ist Daten zu verarbeiten oder zu senden. Die Kindprozesse werden mit dem Wert 98 versehen um eine unterbrechungsfreie Übertragung des Elternprozesses zu garantieren. 
