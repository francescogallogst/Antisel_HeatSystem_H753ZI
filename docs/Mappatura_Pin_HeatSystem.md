# Mappatura pin Heat System — NUCLEO-H753ZI

Scheda **fisicamente separata** dal Nucleo AntiSEL (192.168.1.100, porta
7755) — vedi `AntiSEL/docs/Proposta_HeatSystem_RTU_PID.md`. Tutta la logica
gira su questo MCU single-core.

## Segnali applicativi

| Segnale | Pin STM32 | Periferica / config | Ruolo |
|---|---|---|---|
| MAX31865 CS | **PE4** | GPIO output PP, riposo **HIGH** (attivo basso) | Chip select SPI del driver RTD |
| MAX31865 DRDY | **PE5** | EXTI5, fronte **discesa**, pull-up esterno (open-drain) | Data-ready (letto per polling in `MAX31865_DataReady()`, non collegato a un ISR) |
| MAX31865 SCK | **PB10** | SPI2_SCK, AF5 | |
| MAX31865 MISO | **PC2_C** | SPI2_MISO, AF5 | |
| MAX31865 MOSI | **PC3_C** | SPI2_MOSI, AF5 | SPI2: 8 bit, mode 1 (CPOL=0/CPHA=1), prescaler /64 (3.125 MBit/s, da CubeMX) |
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

## Periferiche di supporto

| Periferica | Config | Note |
|---|---|---|
| IWDG1 | Prescaler 256, Reload 249 → timeout ~2 s | Avviato **dopo** tutte le init lente (LWIP/BSP), appena prima del `while(1)`, altrimenti scade durante l'autonegoziazione PHY e causa un reset loop |
| LED verde (LD1) | BSP | Heartbeat: acceso all'ingresso nel loop, poi lampeggio ogni 500 ms — segnale visivo che il firmware è vivo, indipendente dal debugger |

## Corrispondenza nome ↔ `#define` nel firmware

| Segnale | Define (`Core/Inc/main.h`) |
|---|---|
| MAX31865 CS | `MAX31865_CS_Pin` / `MAX31865_CS_GPIO_Port` |
| MAX31865 DRDY | `MAX31865_DRDY_Pin` / `MAX31865_DRDY_GPIO_Port` |

## Ancora da assegnare (non presente in questa scheda)

- Nessun pin di enable/fault dedicato per lo stadio di potenza: la
  proposta (§3.1/§4.1) non ne menziona uno esplicito — se lo stadio
  scelto ne richiede uno, va aggiunto in CubeMX e documentato qui.
