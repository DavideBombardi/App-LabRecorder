"""
LabRecorder API — controllo completo via TCP + stdout
Compatibile con il fork Adaptronics di LabRecorder.

Uso minimo:
    lr = LabRecorderAPI(r"C:\path\to\LabRecorder.exe")
    lr.on_done = lambda data: print("File salvato:", data["path"])
    lr.launch()
    run = lr.set_metadata(acquisition="91912", participant="P001", operator="Mario Rossi")
    print("Run:", run)
    lr.start()
    # ... aspetta fine test ...
    lr.stop()
    lr.close()
"""

import json
import socket
import subprocess
import threading
import time
from typing import Callable, Optional


class LabRecorderAPI:

    def __init__(self, exe_path: str, host: str = "localhost", port: int = 22345):
        self.exe_path = exe_path
        self.host     = host
        self.port     = port

        self._proc:       Optional[subprocess.Popen] = None
        self._sock:       Optional[socket.socket]    = None
        self._sockfile                               = None
        self._sock_lock   = threading.Lock()
        self._running     = False

        # --- Callback opzionali ---
        # on_started(data: dict)  — chiamato quando la registrazione parte
        #   data contiene: status, run, path
        self.on_started: Optional[Callable] = None

        # on_done(data: dict)     — chiamato quando la registrazione finisce
        #   data contiene: status, path, run, cad_id, patch_id, operator,
        #                  material_id, test_id, note, perno_*, start_time,
        #                  end_time, hostname
        self.on_done: Optional[Callable] = None

        # on_log(level: str, msg: str)  — log da LabRecorder
        #   level: "INFO", "WARNING", "CRITICAL", "FATAL"
        self.on_log: Optional[Callable] = None

        # on_streams_status(data: dict)  — chiamato dopo un comando "update"
        #   data contiene: available (list[str]), missing (list[str])
        self.on_streams_status: Optional[Callable] = None

        # on_crash(exit_code: int)  — chiamato se LabRecorder muore inaspettatamente
        self.on_crash: Optional[Callable] = None

    # ------------------------------------------------------------------
    # Ciclo di vita
    # ------------------------------------------------------------------

    def launch(self, cfg_path: str = None, timeout: float = 5.0):
        """
        Lancia LabRecorder.exe, aspetta che il TCP sia pronto e si connette.

        Args:
            cfg_path: percorso al file .cfg (opzionale, usa il default se None)
            timeout:  secondi massimi di attesa per la connessione TCP
        """
        cmd = [self.exe_path]
        if cfg_path:
            cmd += ["-c", cfg_path]

        self._proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,   # stderr unito a stdout: tutto leggibile
        )

        self._running = True
        t = threading.Thread(target=self._read_stdout, daemon=True, name="LR-stdout")
        t.start()

        self._connect(timeout)

    def connect(self, timeout: float = 5.0):
        """
        Connette a un LabRecorder già in esecuzione (senza lanciarlo).
        Usa questa variante se LabRecorder è già aperto.
        """
        self._running = True
        self._connect(timeout)

    def close(self):
        """Chiude socket e termina il processo se lanciato da noi."""
        self._running = False
        if self._sockfile:
            try: self._sockfile.close()
            except: pass
        if self._sock:
            try: self._sock.close()
            except: pass
        if self._proc:
            try: self._proc.terminate()
            except: pass

    # ------------------------------------------------------------------
    # Comandi principali
    # ------------------------------------------------------------------

    def set_metadata(self, **kwargs) -> int:
        """
        Setta i metadati e restituisce la run auto-selezionata su disco.

        Parametri accettati (tutti opzionali, passali come keyword args):
            acquisition      — ID CAD  (finisce in cad_id nell'XDF)
            participant      — ID Patch (finisce in patch_id nell'XDF)
            operator         — nome operatore
            material         — material ID
            test             — test ID
            note             — note pre-test (testo libero, no '}')
            perno_materiale  — materiale perno
            perno_diametro   — diametro perno
            perno_numero     — numero perni
            perno_posizione  — posizione perno
            run              — forza la run a un valore specifico
            session          — label sessione (parte del path BIDS)
            root             — cartella radice registrazioni
            modality         — modalità BIDS (eeg, meg, ...)

        Returns:
            run: int — run corrente dopo l'applicazione dei metadati
        """
        if kwargs:
            options = "".join(f"{{{k}:{v}}}" for k, v in kwargs.items())
            self._send("filename " + options)
        return self.get_run()

    def get_run(self) -> int:
        """Restituisce la run corrente (senza avviare niente)."""
        resp = self._send("status")
        return json.loads(resp)["run"]

    def start(self):
        """
        Avvia la registrazione.
        Chiama set_metadata() prima per assicurarti che i metadati siano corretti.
        LabRecorder selezionerà tutti gli stream disponibili automaticamente.
        Ascolta on_started per conferma con run e path definitivi.
        """
        self._send("start")

    def stop(self):
        """
        Ferma la registrazione.
        Ascolta on_done per ricevere il JSON completo con path e metadati.
        """
        self._send("stop")

    def refresh_streams(self):
        """Aggiorna la lista degli stream LSL disponibili."""
        self._send("update")

    def select_all_streams(self):
        self._send("select all")

    def select_no_streams(self):
        self._send("select none")

    # ------------------------------------------------------------------
    # Monitoraggio processo
    # ------------------------------------------------------------------

    @property
    def is_alive(self) -> bool:
        """True se LabRecorder è ancora in esecuzione."""
        if self._proc is None:
            return False
        return self._proc.poll() is None

    # ------------------------------------------------------------------
    # Internals
    # ------------------------------------------------------------------

    def _connect(self, timeout: float):
        deadline = time.time() + timeout
        last_err = None
        while time.time() < deadline:
            try:
                self._sock = socket.create_connection((self.host, self.port), timeout=2.0)
                self._sockfile = self._sock.makefile("r", encoding="utf-8")
                return
            except OSError as e:
                last_err = e
                time.sleep(0.2)
        raise ConnectionError(
            f"Impossibile connettersi a LabRecorder su {self.host}:{self.port} "
            f"entro {timeout}s — {last_err}"
        )

    def _send(self, cmd: str) -> str:
        """Manda un comando TCP e legge la risposta (OK o JSON). Thread-safe."""
        with self._sock_lock:
            self._sock.sendall((cmd + "\n").encode())
            return self._sockfile.readline().strip()

    def _read_stdout(self):
        """Thread daemon che legge stdout di LabRecorder e chiama i callback."""
        for raw in iter(self._proc.stdout.readline, b""):
            if not self._running:
                break
            line = raw.decode("utf-8", errors="replace").strip()
            self._dispatch(line)

        # Processo terminato
        self._running = False
        exit_code = self._proc.returncode if self._proc else -1
        if self.on_crash and exit_code not in (0, None):
            self.on_crash(exit_code)

    def _dispatch(self, line: str):
        PREFIX_STARTED = "[LabRecorder] RECORDING_STARTED:"
        PREFIX_DONE    = "[LabRecorder] RECORDING_DONE:"
        PREFIX_STREAMS = "[LabRecorder] STREAMS_STATUS:"
        PREFIX_LOG     = "[LabRecorder] LOG:"

        try:
            if line.startswith(PREFIX_STARTED):
                data = json.loads(line[len(PREFIX_STARTED):])
                if self.on_started:
                    self.on_started(data)

            elif line.startswith(PREFIX_DONE):
                data = json.loads(line[len(PREFIX_DONE):])
                if self.on_done:
                    self.on_done(data)

            elif line.startswith(PREFIX_STREAMS):
                data = json.loads(line[len(PREFIX_STREAMS):])
                if self.on_streams_status:
                    self.on_streams_status(data)

            elif line.startswith(PREFIX_LOG):
                rest  = line[len(PREFIX_LOG):]
                parts = rest.split(":", 1)
                level = parts[0] if len(parts) == 2 else "INFO"
                msg   = parts[1] if len(parts) == 2 else rest
                if self.on_log:
                    self.on_log(level, msg)

        except Exception as e:
            if self.on_log:
                self.on_log("WARNING", f"[labrecorder_api] errore parsing riga stdout: {e} — {line!r}")


# ----------------------------------------------------------------------
# Esempio d'uso completo
# ----------------------------------------------------------------------
if __name__ == "__main__":
    import sys

    EXE  = r"C:\path\to\LabRecorder.exe"
    CFG  = r"C:\path\to\LabRecorder.cfg"
    PORT = 22345

    done_event = threading.Event()

    lr = LabRecorderAPI(EXE, port=PORT)

    lr.on_log     = lambda level, msg: print(f"[{level}] {msg}")
    lr.on_started = lambda data: print(f"▶ Registrazione avviata — run {data['run']} → {data['path']}")
    lr.on_done    = lambda data: (print(f"■ Fine — {data['path']}"), done_event.set())
    lr.on_crash   = lambda code: (print(f"CRASH! Exit code: {code}"), sys.exit(1))

    print("Avvio LabRecorder...")
    lr.launch(CFG)

    # Setta metadati → restituisce run auto-selezionata
    run = lr.set_metadata(
        acquisition     = "91912",
        participant     = "P001",
        operator        = "Mario Rossi",
        material        = "acciaio",
        test            = "T-001",
        note            = "nota pre-test",
        perno_materiale = "acciaio",
        perno_diametro  = "M6",
        perno_numero    = "2",
        perno_posizione = "fronte",
    )
    print(f"Prossima run: {run}")

    input("Premi INVIO per avviare la registrazione...")
    lr.start()

    input("Premi INVIO per fermare la registrazione...")
    lr.stop()

    # Aspetta il JSON di completamento (max 10s)
    done_event.wait(timeout=10)

    lr.close()
