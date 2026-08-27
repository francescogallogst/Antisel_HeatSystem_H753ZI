# Mappatura pin Heat System — NUCLEO-H753ZI

Scheda **fisicamente separata** dal Nucleo AntiSEL (192.168.1.100, porta
7755) — vedi `AntiSEL/docs/Proposta_HeatSystem_RTU_PID.md`. Tutta la logica
gira su questo MCU single-core.

## Segnali applicativi

> ⚠️ Il sensore montato è un **Adafruit MAX31856** (termocoppia Tipo T), non
> un MAX31865 (RTD): protocollo/registri dei due chip sono incompatibili
> (driver in `max31856.c/.h`). I nomi dei segnali/define sotto sono
> l'etichetta ereditata da CubeMX (assegnata quando il progetto usava ancora
> il MAX31865) e non sono stati rinominati per non dover ripassare da una
> rigenerazione `.ioc`; il pinout fisico (SPI3 su PB3/PB4/PB5, CS su PE4) è
> comunque invariato e valido anche per il MAX31856.

| Segnale | Pin STM32 | Periferica / config | Ruolo |
|---|---|---|---|
| MAX31865 CS | **PE4** | GPIO output PP, riposo **HIGH** (attivo basso) | Chip select SPI del driver termocoppia |
| MAX31865 SCK | **PB3** | SPI3_SCK, AF6 | Pin condiviso con JTDO/TRACESWO (JTAG) — libero perché il progetto non configura il debug in modalità JTAG (SWD standard a 2 fili) |
| MAX31865 MISO | **PB4** | SPI3_MISO, AF6 | Pin condiviso con NJTRST (JTAG) — libero per lo stesso motivo di cui sopra |
| MAX31865 MOSI | **PB5** | SPI3_MOSI, AF7 | SPI3: 8 bit, mode 1 (CPOL=0/CPHA=1), prescaler /64 (3.125 MBit/s, da CubeMX). Sostituisce la precedente mappatura su SPI2 (PB10/PC2_C/PC3_C) per evitare i pin "_C" (switch analogico) e il LED LD3 (PB14) |
| Heater PWM | **PA6** | TIM3_CH1, AF2 | Pilota lo stadio di potenza (MOSFET/SSR — tipo ancora TBD, §7 proposta) verso le resistenze |

## Rete

| Parametro | Valore |
|---|---|
| IP statica | `192.168.1.101` |
| Netmask | `255.255.255.0` |
| Porta server RTU/PID | `7756` |
| MAC | `00:80:E1:xx:yy:zz` (gli ultimi 3 byte sono generati dinamicamente a runtime a partire dall'UID univoco del microcontrollore in `ethernetif.c`, garantendo l'assenza di conflitti ARP sulla LAN) |

## Clock

| Parametro | Valore |
|---|---|
| HSE | 8 MHz (MCO ST-LINK, bypass) — **non** 25 MHz come inizialmente generato da CubeMX |
| SYSCLK | 480 MHz (PLL1 M=4 N=480 P=2, VOS0, Flash Latency 4WS) |
| AHB | 240 MHz |
| APBx | 120 MHz |
| Funzione | `SystemClock_Config_480MHz()` in `Core/Src/main.c`, chiamata al posto della `SystemClock_Config()` generata — stesso schema già usato in AntiSEL |
| Alimentazione core | `PWR_LDO_SUPPLY` (non SMPS) — la NUCLEO-H753ZI non è popolata per l'alimentazione diretta da SMPS |

> ⚠️ **Trappola ad ogni rigenerazione CubeMX**: la chiamata a
> `SystemClock_Config_480MHz()` in `main()` sta fuori dai blocchi
> `USER CODE`, quindi CubeMX la sovrascrive sempre con la
> `SystemClock_Config()` di default ad ogni "Generate Code" — va
> ripristinata a mano dopo ogni rigenerazione. `RCC.SupplySource` è
> invece salvato correttamente nel `.ioc` come `PWR_LDO_SUPPLY` (fix
> applicato in data odierna, prima riportava erroneamente
> `PWR_DIRECT_SMPS_SUPPLY`), quindi la `SystemClock_Config()` generata
> di default ora eredita già il valore giusto.

## Periferiche di supporto

| Periferica | Config | Note |
|---|---|---|
| IWDG1 | Prescaler 256, Reload 249 → timeout ~2 s | Avviato **dopo** tutte le init lente (LWIP/BSP), appena prima del `while(1)`, altrimenti scade durante l'autonegoziazione PHY e causa un reset loop |
| LED verde (LD1) | BSP | Heartbeat: acceso all'ingresso nel loop, poi lampeggio ogni 500 ms — segnale visivo che il firmware è vivo, indipendente dal debugger |

## Corrispondenza nome ↔ `#define` nel firmware

| Segnale | Define (`Core/Inc/main.h`) |
|---|---|
| MAX31865 CS | `MAX31865_CS_Pin` / `MAX31865_CS_GPIO_Port` |

## Ancora da assegnare (non presente in questa scheda)

- Nessun pin di enable/fault dedicato per lo stadio di potenza: la
  proposta (§3.1/§4.1) non ne menziona uno esplicito — se lo stadio
  scelto ne richiede uno, va aggiunto in CubeMX e documentato qui.
