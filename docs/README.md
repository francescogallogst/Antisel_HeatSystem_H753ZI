# Antisel_HeatSystem — panoramica

Firmware del dispositivo RTU/PID Heat System descritto in
`AntiSEL/docs/Proposta_HeatSystem_RTU_PID.md`: legge un **MAX31865** (RTD)
e pilota in **PWM proporzionale** delle resistenze di riscaldamento,
esponendo un protocollo TCP testuale su `192.168.1.101:7756` verso il
pannello "PID CTRL + RTU" di `AntiSel_GUI/antisel_dashboard_eth.py`.

Scheda **NUCLEO-H753ZI separata** dalla scheda AntiSEL (SEL/HCE,
`192.168.1.100:7755`, repo `AntiSEL/`) — nessuna relazione/dipendenza col
suo firmware. Tutta la logica gira su questo MCU single-core.

## Documenti

- [Mappatura_Pin_HeatSystem.md](Mappatura_Pin_HeatSystem.md) — pin, clock,
  rete
- [RTU_PID_Protocollo.md](RTU_PID_Protocollo.md) — contratto TCP con la GUI
- `AntiSEL/docs/Proposta_HeatSystem_RTU_PID.md` — proposta architetturale
  originale (Opzione A vs B, decisioni aperte §7)

## Moduli applicativi (`Core/{Src,Inc}`)

| Modulo | Ruolo |
|---|---|
| `max31865.c/h` | Driver RTD via SPI2: init, lettura codice/temperatura, fault |
| `heater_ctrl.c/h` | PID, scrittura PWM (TIM3_CH1), safety check locale, watchdog di disconnessione, modi OFF/MANUAL/AUTO/FAULT |
| `rtu_protocol.c/h` | Server TCP raw LwIP sulla porta 7756, dispatch comandi |

## Log del bring-up

Problemi reali trovati e corretti durante la messa in funzione su hardware
(non solo scelte di design — bug concreti):

1. **ADC1/DAC1 assenti, clock diverso da AntiSEL** — atteso: è una scheda
   fisicamente diversa, non un'evoluzione dell'MCU di AntiSEL.
2. **`RCC.HSE_VALUE=25000000`** nel `.ioc`/`SystemClock_Config()`
   generato, ma la scheda è una NUCLEO-144 stock con HSE reale a 8 MHz
   (MCO ST-LINK). Con l'input sbagliato la PLL veniva calcolata su un
   range VCO non valido → instabilità che impediva anche l'aggancio del
   debugger via SWD dopo il flash. **Fix**: portata la
   `SystemClock_Config_480MHz()` già collaudata su AntiSEL (stesso MCU).
3. **`MX_IWDG1_Init()` chiamato troppo presto** (subito dopo
   `MX_GPIO_Init()`), prima di `MX_LWIP_Init()` (autonegoziazione PHY,
   spesso oltre i ~2 s di timeout dell'IWDG) → reset loop continuo prima
   di raggiungere il `while(1)` dove veniva rinfrescato. **Fix**: spostato
   subito prima del loop principale, a inizializzazione completata.
4. **SPI2 (MAX31865)**: `DataSize=SPI_DATASIZE_4BIT` (serve 8 bit),
   `BaudRatePrescaler` da 100 MHz effettivi (il MAX31865 supporta ≤5 MHz),
   e **fase sbagliata** (CPHA=0 generato, il chip richiede mode 1,
   CPHA=1). Tutti e tre corretti.
5. **MAC Ethernet duplicato**: stesso MAC hardcoded `00:80:E1:00:00:00`
   copiato dal template CubeMX, identico a quello della scheda AntiSEL.
   **Fix**: gli ultimi 3 byte del MAC sono ora generati dinamicamente
   a partire dall'UID del microcontrollore per evitare conflitti ARP.
6. **`MX_LWIP_Process()` mai chiamata** nel loop principale — senza,
   lo stack LwIP non processa mai RX/timeout TCP, quindi *nessuna*
   connessione TCP può funzionare a prescindere dal server applicativo.
   Aggiunta al `while(1)`.
7. **MAX31865_CS (PE4)** generato con livello iniziale basso (chip select
   attivo-basso deve riposare alto). **MAX31865_DRDY (PE5)** generato con
   trigger `RISING` invece di `FALLING` (il pin è attivo-basso). Entrambi
   corretti.
8. **Overflow CCR TIM3 a duty 100%**: `Period+1` (65536) scritto in un
   registro a 16 bit trabocca a 0 → 100% richiesto diventava 0% erogato.
   Clampato a `Period`.
9. **Flag di freschezza RTD**: poteva risultare erroneamente "vero" nei
   primi istanti dopo il boot, prima di qualunque lettura reale del
   MAX31865. Aggiunto un flag esplicito "letto almeno una volta".
10. **Aggiunta LED di stato**: Abilitato il LED verde (LD1) come heartbeat per certificare visivamente il corretto funzionamento del main loop (lampeggia ogni 500 ms).
11. **Pulizia build**: Eliminati file temporanei non necessari e risolti i warning per garantire una compilazione completamente pulita.

## Parametri non tarati (placeholder — §7 della proposta)

Vedi tabella in [RTU_PID_Protocollo.md](RTU_PID_Protocollo.md#parametri-ancora-da-tarare-7-della-proposta):
guadagni PID, soglia di cutoff termico, timeout watchdog, wiring RTD
(2/3/4 fili), tipo di stadio di potenza (MOSFET vs SSR) e relativa
frequenza PWM di TIM3 (attualmente ai default CubeMX, ~3.7 kHz).
