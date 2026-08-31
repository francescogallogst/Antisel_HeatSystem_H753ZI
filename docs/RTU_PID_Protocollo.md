# Protocollo RTU/PID Heat System — v1 (placeholder)

Documento di contratto tra **firmware** di `Antisel_HeatSystem` (server TCP)
e il pannello "PID CTRL + RTU" della dashboard Python
(`AntiSel_GUI/antisel_dashboard_eth.py`). Stesso stile di
[AntiSEL_Protocollo_Comandi.md](../../AntiSel_GUI/docs/AntiSEL_Protocollo_Comandi.md),
ma per il dispositivo Heat System, indipendente dalla scheda AntiSEL.

Vedi anche `AntiSEL/docs/Proposta_HeatSystem_RTU_PID.md` per il contesto
architetturale completo (§5.3 in particolare).

- **Trasporto:** TCP, `192.168.1.101:7756`
- **Encoding:** ASCII, un comando per riga, terminatore `\r\n`
- **Risposte:** una riga `OK ...` oppure `ERR <causa>`
- **Parsing GUI:** `_parse_kv()` estrae qualunque token `CHIAVE=VALORE`
  separato da spazi nella riga — l'ordine e il prefisso `OK`/testo
  circostante non contano, contano solo le coppie chiave=valore

Implementato in `Core/Src/rtu_protocol.c`.

---

## Comandi

| Comando | Argomento | Risposta OK | Note |
|---|---|---|---|
| `GET TEMP` | — | `OK TEMP=<°C, 1 decimale>` | `ERR RTD_STALE` se nessuna lettura RTD valida negli ultimi `rtd_stale_ms` (default 2000 ms, vedi `heater_ctrl.h`) |
| `GET PID` | — | `OK PID PWM=<%, 1 decimale> STATE=<OFF\|MANUAL\|AUTO\|FAULT>` | |
| `SET SETPOINT_C <v>` | setpoint [°C] | `OK SETPOINT_C=<v>` | Porta il controllore in modo **AUTO** e resetta il timestamp del watchdog di disconnessione. Se lo stato torna subito a `FAULT` al successivo `GET PID`, il comando è stato accettato ma `safety_check()` lo ha rilatchato nello stesso ciclo — il guasto sottostante (MAX31856) è ancora presente |
| `ACK FAULT` | — | `OK ACK STATE=<OFF\|FAULT>` | Esce da `FAULT` verso `OFF` **solo se** la condizione di guasto non si ripresenta al `safety_check()` successivo; altrimenti la risposta stessa può già mostrare `STATE=FAULT` |
| `GET RAW` | — | `OK RAW CR0=<hex> CR1=<hex> MASK=<hex> SR=<hex> LTCB=<hex,3 byte> SPI_OK=<0\|1>` | Dump diagnostico dei registri MAX31856 via SPI1. `SPI_OK` indica solo che le transazioni HAL non hanno avuto timeout, **non** che i dati siano elettricamente validi |
| `GET GPIO` | — | `OK GPIO CS=<0\|1> SCK=<0\|1> MISO=<0\|1> MOSI=<0\|1>` | Livello elettrico istantaneo (IDR) dei 4 pin, letto senza toccare la configurazione SPI1 — utile per verificare il bus senza strumenti esterni |
| `TEST SCK` | — | `OK TEST_SCK_START` poi (dopo ~2 s) `OK TEST_SCK_DONE` | **Bloccante**: scavalca SPI1 e pilota SCK (PA5) come GPIO in toggle a ~500 Hz per bring-up con oscilloscopio, poi ripristina AF5/SPI1. Sospende `Heater_Service()` per 2 s (refresh IWDG incluso nel ciclo) |
| altro | — | `ERR UNKNOWN` | |

La GUI interroga `GET TEMP` e `GET PID` ogni `RTU_POLL_S` (1.0 s, definito
lato GUI) e invia `SET SETPOINT_C` quando l'utente preme "Imposta" nel
pannello RTU/PID.

## Stati (`STATE=`)

| Stato | Significato |
|---|---|
| `OFF` | Riscaldatore spento, nessun comando attivo |
| `MANUAL` | Duty impostato manualmente (non ancora raggiungibile dal protocollo attuale — riservato per estensioni future) |
| `AUTO` | PID attivo, insegue `SETPOINT_C` |
| `FAULT` | Safety check ha rilevato un problema (fault MAX31856, sovratemperatura, RTD stantia) — PWM forzato a 0%, resta in FAULT finché non arriva un `ACK FAULT` |

## Sicurezza locale (indipendente dal protocollo)

Implementata in `heater_ctrl.c`, non richiede comandi dalla GUI:

- **Safety check**: se il MAX31856 segnala un fault (incluso open-circuit, 
  attivato via `OCFAULT`), se la temperatura supera `cutoff_c` (default 90.0 °C), 
  o se la lettura RTD è stantia, il sistema rileva un potenziale guasto.
  Per prevenire falsi positivi dovuti a transienti EMI generati dalla 
  commutazione del Relè a Stato Solido (SSR), il controllo è **rate-limited a 4 Hz** 
  e richiede che l'errore persista per **3 cicli consecutivi (750 ms)**. Solo allora 
  il PWM viene forzato a 0% e lo stato passa a `FAULT`. `ACK FAULT` riporta a `OFF` 
  solo se l'errore fisico è risolto.
- **Watchdog di disconnessione**: se in modo `AUTO` non arriva nessun
  comando (`SET SETPOINT_C` o polling tramite `GET TEMP`/`GET PID`) entro `watchdog_ms` (default 5000 ms)
  il riscaldatore torna a `OFF` da solo.

## Estensioni previste (non ancora implementate)

Come da proposta §5.3, il protocollo è pensato per essere esteso restando
compatibile col parsing esistente:

- `SET MODE <OFF|MANUAL|AUTO>` — cambio modo esplicito
- `GET FAULT` — dettaglio bit di fault MAX31856 già decodificati
  (`MAX31856_Fault_t`), oggi ricavabili solo indirettamente da `GET RAW`

## Parametri ancora da tarare (§7 della proposta)

Questi valori sono **placeholder conservativi**, non tarati su hardware
reale — vedi i commenti in `heater_ctrl.c`:

| Parametro | Default attuale | Dove |
|---|---|---|
| Guadagni PID (Kp, Ki, Kd) | 5.0 / 0.05 / 0.0 | `heater_ctrl.c`, `cal` |
| PID Anti-windup (Max Integrale) | 100% (aggiornato) | `heater_ctrl.c` (modificato da 50% a 100% per azzerare l'errore a regime) |
| Frequenza PWM (Time Proportioning) | 1 Hz | `tim.c`, `MX_TIM4_Init` (adatto per SSR zero-crossing, evita spegnimenti spuri) |
| Soglia di cutoff termico | 90.0 °C | `heater_ctrl.c`, `cal.cutoff_c` |
| Timeout watchdog disconnessione | 5000 ms | `heater_ctrl.c`, `cal.watchdog_ms` |
| Staleness RTD | 2000 ms | `heater_ctrl.c`, `cal.rtd_stale_ms` |
| Termocoppia | Tipo T, filtro rete 50/60 Hz configurabile | `max31856.c`, `cal` |
