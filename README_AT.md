# README_AT — Registro Modifiche Adaptronics

Questo file traccia tutte le modifiche apportate al fork Adaptronics di LabRecorder
rispetto al repository originale (upstream): https://github.com/labstreaminglayer/App-LabRecorder

Ogni modifica è contrassegnata nel codice con il commento `[ADAPTRONICS]`.

---

## Obiettivo del fork

Adattare LabRecorder a un contesto di test industriale su patch.
Le modifiche principali riguardano:
- Struttura cartelle: `ID_CAD / ID_Patch / run_001.xdf`
- Pannello metadati aggiuntivo (Operator, Material ID, Test ID, Note, gruppo Perno)
- Metadati scritti nella FileHeader XDF nel blocco `<adaptronics>`
- Menu a tendina cascata popolati da CSV (`LR_Runtime_Entries.csv`), con filtro per operatore
- Username Windows letto automaticamente per pre-selezionare l'operatore
- Notifica completamento registrazione alla GUI Python via stdout + file JSON di fallback
- Adattamento dell'interfaccia grafica al workflow degli operatori

---

## Distribuzione — struttura cartella operatore

Dopo la compilazione (GitHub Actions → artefatto `LabRecorder.exe`),
la cartella consegnata all'operatore deve contenere:

```
LabRecorder/
├── LabRecorder.exe          ← eseguibile compilato (da GitHub Actions)
├── LabRecorder.cfg          ← rinominato da LabRecorder_AT.cfg
├── LR_Runtime_Entries.csv       ← lista CAD, Patch, Operator, Material, Test, Perno
└── liblsl.dll               ← libreria LSL (dalla release ufficiale LSL)
```

> `liblsl.dll` si trova nelle [release ufficiali di liblsl](https://github.com/sccn/liblsl/releases).
> Scaricare la versione Windows x64 e copiare la DLL accanto all'exe.

**Avvio automatico della configurazione:**
`LabRecorder.exe` cerca all'avvio un file `LabRecorder.cfg` nella stessa cartella.
Rinominare `LabRecorder_AT.cfg` → `LabRecorder.cfg` prima della distribuzione.

**Struttura delle registrazioni** (definita in `LabRecorder.cfg`):
```
C:/Registrazioni/Adaptronics/
└── IDCAD_91912/
    └── PATCH_PATCH001/
        ├── run_001.xdf
        ├── run_002.xdf
        └── ...
```

---

## Modifiche effettuate

### 1. `src/mainwindow.ui` — Etichette rinominate e widget nascosti

**Cosa:** rinominati i testi delle etichette dei campi esistenti e nascosti quelli non rilevanti.

| Campo originale | Nuovo nome | Segnaposto | Stato |
|----------------|------------|------------|-------|
| Acq. | ID CAD | `%a` | visibile |
| Session | — | `%s` | **nascosto** (si sovrappone a ID CAD) |
| Participant | ID Patch | `%p` | visibile |
| Counter | Run | `%n` | visibile |
| Block/Task | — | `%b` | nascosto |
| Modality | — | `%m` | nascosto |
| BIDS checkbox | — | — | nascosto |

**Perché:** adattare la terminologia all'uso industriale. I widget sono nascosti con
`visible=false`, NON rimossi: il codice C++ che li referenzia continua a compilare.

**Conflitti futuri:** se l'upstream modifica queste label, il conflitto è visibile.
Mantenere i nuovi nomi e le proprietà `visible=false`.

---

### 2. `src/mainwindow.ui` — Campi convertiti da QLineEdit a QComboBox editabile

**Cosa:** i campi `lineEdit_acq` e `lineEdit_participant` sono stati
convertiti da `QLineEdit` a `QComboBox` con proprietà `editable = true`.
(Anche `lineEdit_session` è QComboBox, ma nascosto.)

**Perché:** permette all'operatore di scegliere da un menu a tendina (popolato da CSV)
oppure di digitare liberamente un valore non presente nella lista.

**Conflitti futuri:** se l'upstream modifica questi widget, mantenere il tipo `QComboBox`
e la proprietà `editable = true`.

---

### 3. `src/mainwindow.ui` — Pannello metadati sessione (colonna destra)

**Cosa:** aggiunto un `QGroupBox` "Metadati Sessione" come terza colonna a destra.
Layout del pannello (dall'alto):

| Widget | Label | Tipo | Note |
|--------|-------|------|------|
| `checkBox_showAll` | Mostra tutto | QCheckBox | disabilita filtro operatore su tutte le tendine |
| `comboBox_meta_operator` | Operator | QComboBox editabile | sempre completa, pre-selezionata con username Windows |
| `comboBox_meta_material` | Material ID | QComboBox editabile | CSV TYPE=MATERIAL, filtrata per operatore |
| `comboBox_meta_test` | Test ID | QComboBox editabile | CSV TYPE=TEST, filtrata per operatore |
| `groupBox_perno` | Perno | QGroupBox | contiene i 4 campi sotto |
| `comboBox_meta_perno_materiale` | Materiale | QComboBox editabile | CSV TYPE=PERNO_MATERIALE, indipendente da CAD e operatore |
| `comboBox_meta_perno_diametro` | Diametro | QComboBox editabile | CSV TYPE=PERNO_DIAMETRO, indipendente da CAD e operatore |
| `comboBox_meta_perno_numero` | N. perni | QComboBox editabile | CSV TYPE=PERNO_NUMERO, indipendente da CAD e operatore |
| `comboBox_meta_perno_posizione` | Posizione | QComboBox editabile | CSV TYPE=PERNO_POSIZIONE, indipendente da CAD e operatore |
| `plainTextEdit_meta_note` | Note | QPlainTextEdit | testo libero multilinea, nessun filtro |

Altre modifiche UI:
- `groupBox_metadata` posizionato a `row=3, column=1` di `gridLayout_master`: sotto ai controlli file, nella stessa colonna — layout a 2 colonne identico all'upstream (più facile da mergiare)
- `groupBox_streams` con `rowspan=2` (righe 2-3): si espande verticalmente fino a coprire anche la riga dei metadati
- `wordWrap=true` su `locationLabel`: il percorso lungo va a capo invece di espandere la colonna

**Conflitti futuri:** blocco completamente nuovo, nessun elemento upstream toccato.

---

### 4. `src/mainwindow.cpp` — Logica dropdown, filtro operatore, notifica completamento

**Cosa A — segnali e accesso ai valori (QComboBox):**
- Segnali: `QLineEdit::editingFinished` → `QComboBox::currentTextChanged`
- Lettura: `.text()` → `.currentText()` / Scrittura: `.setText()` → `.setCurrentText()`
- Punti toccati: costruttore, `replaceFilename()`, `buildBidsTemplate()`, `rcsUpdateFilename()`

**Cosa B — cascata ID CAD → ID Patch:**
Nel costruttore, quando cambia ID CAD viene ripopolato ID Patch con i figli del CAD dichiarati nel CSV,
rispettando il filtro operatore corrente. I campi Perno sono indipendenti dal CAD selezionato.

**Cosa C — raccolta metadati in `startRecording()`:**
Tutti i valori dell'interfaccia vengono raccolti in `std::map<std::string,std::string>`
e passati a `recording` → `XDFWriter` → FileHeader XDF. Campi:
`cad_id`, `patch_id`, `operator`, `material_id`, `test_id`, `note`,
`perno_materiale`, `perno_diametro`, `perno_numero`, `perno_posizione`, `run`, `start_time`.
Path e metadati vengono anche salvati in `lastRecFilename_` / `lastSessionMetadata_`
per la notifica di completamento. Il campo `run` è il valore corrente di `spin_counter`
al momento dell'avvio — finisce nell'XDF, nel JSON stdout e in `last_recording.json`.

**Cosa D — filtro operatore (`loadAtCsv()`):**
- `currentOperator_` = username Windows (`qgetenv("USERNAME")`) letto all'avvio
- Il campo Operator è pre-selezionato con l'username; la lista mostra sempre tutti gli operatori
- Tutte le altre tendine mostrano solo voci senza operatore + voci dell'operatore selezionato
- Cambiare il campo Operator aggiorna istantaneamente le tendine filtrate
- `checkBox_showAll` bypassa il filtro e mostra tutto
- `atFilteredList(key)`: helper `const` che applica il filtro; usa `QSet<QString>` per deduplicare (se un ID appare più volte con operatori diversi, compare una sola volta nella tendina)
- `repopulateAtDropdowns()`: ripopola tutti i dropdown, chiamato all'avvio e al toggle della spunta

**Cosa E — notifica completamento registrazione (`stopRecording()`):**
Quando la registrazione si ferma, `notifyRecordingDone()` esegue due azioni:
1. `std::cout << "[LabRecorder] RECORDING_DONE:{...json...}"` → letto dalla GUI Python via pipe
2. Scrive `last_recording.json` nella cartella dell'exe → fallback garantito su disco

Il JSON contiene tutti i metadati della sessione + il path assoluto del file XDF.
Esempio riga stdout:
```
[LabRecorder] RECORDING_DONE:{"status":"done","path":"C:/Registrazioni/.../run_001.xdf","cad_id":"91912",...}
```

**Cosa F — auto-incremento run counter:**
- Allo **Stop**: `spin_counter` viene incrementato di 1 automaticamente
- Al **cambio CAD o Patch**: `buildFilename()` scansiona da 1 con il path completo (StudyRoot + template) e si posiziona sul primo run non esistente → cartella nuova = run_001, cartella già usata = run_N+1
- Bug upstream fixato: `buildFilename()` controllava un path relativo senza StudyRoot, quindi `QFileInfo::exists()` non trovava mai niente e il counter restava bloccato a 1

**Cosa G — popup di conferma prima dello Start:** ~~RIMOSSO in v2~~
Eliminato per compatibilità con il controllo API TCP (il popup bloccava lo start remoto).
Il codice è commentato in `startRecording()` con il marker `[ADAPTRONICS] RIMOSSO (v2)` ed è recuperabile.

**Cosa H — freeze metadati durante la registrazione:**
Allo Start, `groupBox_metadata` viene disabilitato interamente (`setEnabled(false)`) — tutti i campi diventano grigi e non modificabili. Allo Stop viene riabilitato. Questo chiarisce visivamente che i metadati scritti nell'XDF sono quelli selezionati prima dello Start e non possono essere cambiati a registrazione in corso.

**Cosa I — `start_time`, `end_time`, `hostname` nel JSON di completamento:**
- `start_time`: catturato in `startRecording()` al momento della creazione del file, formato ISO 8601
- `end_time`: catturato in `buildCompletionJson()` al momento della notifica di stop
- `hostname`: nome macchina via `QSysInfo::machineHostName()`
Tutti e tre presenti sia nello stdout `RECORDING_DONE` che in `last_recording.json`.

**Cosa L — label "Note pre-test":**
Il campo note è stato rinominato da "Note" a "Note pre-test" nel pannello metadati per chiarire che vanno compilate prima di avviare la registrazione.

**Cosa M — popup note post-test allo Stop:** ~~RIMOSSO in v2~~
Eliminato insieme al popup conferma. Le note post-test non vengono più raccolte (non erano nell'XDF,
solo nel JSON). Il codice è commentato in `stopRecording()` con il marker `[ADAPTRONICS] RIMOSSO (v2)`
ed è recuperabile. Se si vuole raccogliere note post-test, farlo nella GUI Python prima di mandare `stop`.

**Misura di sicurezza — chiusura durante registrazione:**
`closeEvent` ignora l'evento di chiusura (`ev->ignore()`) se `currentRecording` è attivo. La finestra non può essere chiusa durante una registrazione — comportamento upstream già presente, non modificato.

**Conflitti futuri:** tutto il codice [ADAPTRONICS] è in blocchi delimitati.
Le funzioni `atFilteredList`, `repopulateAtDropdowns`, `buildCompletionJson`,
`notifyRecordingDone` sono interamente nuove.

---

### 5. `src/mainwindow.h` — Membri privati Adaptronics

**Cosa:** aggiunti i seguenti membri privati (tutti nel blocco `[ADAPTRONICS]`):

```cpp
// CSV e filtro operatore
QMap<QString, QList<QPair<QString,QString>>> atCsvData_; // (id, operatore)
QString currentOperator_;       // username Windows
bool    atShowAll_ = false;     // stato spunta "Mostra tutto"
QStringList atFilteredList(const QString &key) const;
void repopulateAtDropdowns();
void loadAtCsv(const QString &cfgDir);

// Notifica completamento registrazione
QString lastRecFilename_;
std::map<std::string, std::string> lastSessionMetadata_;
QString buildCompletionJson() const;
void notifyRecordingDone(const QString &cfgDir) const;
```

**Conflitti futuri:** blocco nuovo, nessun membro upstream toccato.

---

### 6. `src/recording.h` + `src/recording.cpp` — Parametro metadata

**Cosa:** aggiunto parametro opzionale `metadata` al costruttore di `recording`:
```cpp
recording(const std::string &filename,
          const std::vector<lsl::stream_info> &streams,
          const std::vector<std::string> &watchfor,
          std::map<std::string, int> syncOptions,
          bool collect_offsets = true,
          const std::map<std::string, std::string> &metadata = {}); // [ADAPTRONICS]
```

**Conflitti futuri:** il parametro è opzionale con default `{}`.
Il codice upstream che chiama `recording(...)` senza metadata continua a funzionare.

---

### 7. `xdfwriter/xdfwriter.h` + `xdfwriter/xdfwriter.cpp` — Scrittura metadati nella FileHeader XDF

**Cosa:** il costruttore di `XDFWriter` accetta un secondo parametro opzionale `metadata`.
Se non è vuota, nel blocco `<info>` della FileHeader XDF viene aggiunto:
```xml
<adaptronics>
  <cad_id>91912</cad_id>
  <patch_id>PATCH001</patch_id>
  <operator>Mario Rossi</operator>
  <material_id>acciaio</material_id>
  <test_id>T-001</test_id>
  <note>Nota libera</note>
  <perno_materiale>acciaio</perno_materiale>
  <perno_diametro>M6</perno_diametro>
  <perno_numero>2</perno_numero>
  <perno_posizione>fronte</perno_posizione>
</adaptronics>
```

I valori vengono XML-escaped (`xml_escape()`) per caratteri speciali (`&`, `<`, `>`, `"`, `'`).

**Conflitti futuri:** parametro opzionale. Logica in blocco `if (!metadata.empty())` separato.

---

## Formato file CSV — `LR_Runtime_Entries.csv`

Il file deve trovarsi nella stessa cartella del `.cfg` (o dell'eseguibile).
Formato: **4 colonne** `TYPE,ID,PARENT_ID,OPERATOR`

> **Attenzione:** il parser usa split semplice su virgola (no RFC-4180). I valori **non devono contenere virgole**. Usare il punto come separatore decimale (es. `67.5mm` non `67,5mm`). Le virgolette nei valori non vengono gestite e rompono il parsing.

- `OPERATOR` è opzionale: **vuoto** = voce visibile a tutti; **valorizzato** = visibile solo a quell'operatore
- `CAD` e `PATCH` possono avere un operatore assegnato (filtro sui CAD visibili)
- `PERNO_*` usano `CAD` come `PARENT_ID` e supportano il filtro operatore

```
TYPE,ID,PARENT_ID,OPERATOR
CAD,91912,,                          ← visibile a tutti
CAD,91914,,Mario Rossi               ← solo Mario Rossi
PATCH,PATCH001,91912,
MATERIAL,acciaio,,
OPERATOR,Mario Rossi,,               ← lista operatori: sempre completa, mai filtrata
TEST,T-001,,
PERNO_MATERIALE,acciaio,91912,       ← visibile a tutti
PERNO_MATERIALE,acciaio speciale,91912,Mario Rossi  ← solo Mario Rossi
```

---

---

### 8. API TCP — Controllo remoto esteso (v2)

**File:** `src/tcpinterface.h` + `src/tcpinterface.cpp` + `src/mainwindow.cpp`

**Cosa:** esteso il protocollo TCP Remote Control Socket con:

#### Nuovi comandi

| Comando | Effetto |
|---------|---------|
| `status` | Risponde `{"run":N}` sulla socket (run counter corrente) |

#### Nuove opzioni del comando `filename`

```
filename {operator:Mario Rossi}{material:acciaio}{test:T-001}{note:testo libero}
         {perno_materiale:acciaio}{perno_diametro:M6}{perno_numero:2}{perno_posizione:fronte}
```

Opzioni complete (tutte opzionali, combinabili in un unico comando):

| Opzione | Widget settato | Finisce nell'XDF |
|---------|----------------|-----------------|
| `acquisition` | `lineEdit_acq` (ID CAD) | `cad_id` |
| `participant` | `lineEdit_participant` (ID Patch) | `patch_id` |
| `operator` | `comboBox_meta_operator` | `operator` |
| `material` | `comboBox_meta_material` | `material_id` |
| `test` | `comboBox_meta_test` | `test_id` |
| `note` | `plainTextEdit_meta_note` | `note` |
| `perno_materiale` | `comboBox_meta_perno_materiale` | `perno_materiale` |
| `perno_diametro` | `comboBox_meta_perno_diametro` | `perno_diametro` |
| `perno_numero` | `comboBox_meta_perno_numero` | `perno_numero` |
| `perno_posizione` | `comboBox_meta_perno_posizione` | `perno_posizione` |
| `run` | `spin_counter` | `run` |
| `session` | `lineEdit_session` | — (path) |
| `root` | `rootEdit` | — (path) |
| `task` | `input_blocktask` | — (path) |
| `modality` | `input_modality` | — (path) |

**Nota sull'ordine:** le opzioni AT vengono applicate in due fasi interne indipendentemente dall'ordine in cui appaiono nel comando: prima `operator` (che triggera `repopulateAtDropdowns`), poi tutti gli altri. Non importa l'ordine nel comando.

**Nota sulla run:** settare `acquisition` o `participant` triggera `buildFilename()` che auto-seleziona la prima run libera su disco. Se si aggiunge anche `{run:N}` nello stesso comando, il valore esplicito sovrascrive l'auto-selezione (viene applicato dopo).

#### Notifiche stdout (v2)

| Messaggio | Quando | Contenuto |
|-----------|--------|-----------|
| `[LabRecorder] RECORDING_STARTED:{...}` | Appena la registrazione parte | `status`, `run`, `path` |
| `[LabRecorder] RECORDING_DONE:{...}` | Quando la registrazione si ferma | tutti i metadati + `run`, `path`, `start_time`, `end_time`, `hostname` |

Entrambi i messaggi vengono anche scritti in `last_recording.json` (solo RECORDING_DONE sovrascrive il file).

#### Flusso Python completo

```python
import socket, json, subprocess

proc = subprocess.Popen(["LabRecorder.exe"], stdout=subprocess.PIPE)

sock = socket.create_connection(("localhost", 22345))
sock.sendall(b'filename {acquisition:91912}{participant:P001}'
             b'{operator:Mario Rossi}{material:acciaio}{test:T-001}'
             b'{note:testo libero}{perno_materiale:acciaio}'
             b'{perno_diametro:M6}{perno_numero:2}{perno_posizione:fronte}\n')
sock.sendall(b'start\n')

# Leggi run effettiva da stdout
for line in proc.stdout:
    if b'RECORDING_STARTED' in line:
        data = json.loads(line.split(b':',1)[1])
        print("Registrazione avviata, run:", data['run'])
        break

# ... a fine test ...
sock.sendall(b'stop\n')

for line in proc.stdout:
    if b'RECORDING_DONE' in line:
        data = json.loads(line.split(b':',1)[1])
        print("File salvato:", data['path'])
        break
```

**Conflitti futuri:** il segnale `status_requested` e le opzioni AT in `rcsUpdateFilename` sono in blocchi `[ADAPTRONICS]`. Il comando `status` in `tcpinterface.cpp` usa `return` prima di `sock->write("OK")` — se l'upstream aggiunge comandi dopo il blocco `select`, verificare che la `return` non salti codice nuovo.

---

## Note generali

- Le modifiche sono progettate per minimizzare i conflitti con l'upstream.
- Ogni modifica C++ è marcata con `// [ADAPTRONICS]` (blocchi con BEGIN/END dove estesi).
- Ogni modifica UI è marcata con `<!-- [ADAPTRONICS] ... -->`.
- In caso di merge conflict: mantenere sempre le righe marcate `[ADAPTRONICS]`.
- I widget nascosti NON vengono mai rimossi: il codice C++ che li referenzia continua a compilare.
