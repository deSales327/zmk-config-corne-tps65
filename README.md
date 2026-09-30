# zmk-config — Corne v3 (6 colunas) + trackpad Azoteq TPS65

Firmware [ZMK](https://zmk.dev) para um Corne v3 split de 6 colunas com um trackpad
**Azoteq TPS65** (controlador IQS550).

| Item        | Valor                                                                 |
| ----------- | --------------------------------------------------------------------- |
| MCU         | Pro Micro nRF52840 "V1940" (clone nice!nano v2) → board `nice_nano//zmk` |
| Central     | Metade **esquerda** (liga ao computador)                              |
| Periférico  | Metade **direita** com o trackpad (enviado à central via `zmk,input-split`) |
| Ecrã        | OLED 128x32 (I2C 0x3C), nas duas metades                              |
| Driver      | cópia de [`essenceotd/zmk_driver_azoteq`](https://github.com/essenceotd/zmk_driver_azoteq) nesta repo (`drivers/`, `dts/`, `zephyr/`), com correção do RDY |
| ZMK         | `main` (Zephyr 4.1)                                                   |

## Ligações do trackpad (Pro Micro da direita)

| TPS65 | Pro Micro | nRF52840 | Notas                               |
| ----- | --------- | -------- | ----------------------------------- |
| VDD   | VCC       | –        | 3.3 V                               |
| GND   | GND       | –        |                                     |
| SDA   | D2        | P0.17    | partilhado com o OLED               |
| SCL   | D3        | P0.20    | partilhado com o OLED               |
| RDY   | D9        | P1.06    | interrupção "dados prontos"         |
| NRST  | D8        | P1.04    | reset (ativo em LOW)                |

D8 e D9 não são usados pelo PCB do Corne v3, por isso liga-os com fio diretamente
aos pinos do Pro Micro. O OLED (0x3C) e o TPS65 (0x74) têm endereços I2C diferentes, por isso
podem partilhar o barramento sem problemas.

> O pino VCC do nice!nano pode ser desligado pelo firmware (`&ext_power`). Se o
> trackpad deixar de responder, confirma que não desligaste a alimentação externa.

## Funcionalidades do trackpad

- Mover o cursor com 1 dedo
- Toque com 1 dedo → clique esquerdo; toque com 2 dedos → clique direito
- Tocar e manter → arrastar
- Scroll com 2 dedos (scroll suave ativo)
- Enquanto usas o trackpad, a layer **Mouse** fica ativa durante 700 ms:
  `S` = clique direito, `D` = clique do meio, `F` = clique esquerdo, `X`/`C` = voltar/avançar

## Estrutura

```
build.yaml                 # alvos: corne_left, corne_right, settings_reset
config/
  west.yml                 # ZMK + módulo do driver Azoteq
  corne.conf               # comum: OLED, pointing
  corne_left.conf          # central: scroll suave, bateria do periférico
  corne_right.conf         # periférico: driver do trackpad
  tps65_split.dtsi         # nó input-split (partilhado)
  corne_left.overlay       # listener do trackpad (+ layer Mouse automática)
  corne_right.overlay      # trackpad no I2C + pinos RDY/NRST
  corne.keymap             # layers Base / Lower / Raise / Mouse
drivers/ dts/ zephyr/      # driver IQS5xx (TPS43/TPS65) com correção do RDY
```

### Correção no driver

O RDY do IQS550 é lido por interrupção de flanco. Se o chip levantar o RDY antes de
a interrupção estar armada (arranque ou saída de suspensão), o flanco perde-se e o
trackpad fica parado até haver um pulso no pino. A cópia do driver nesta repo verifica
o nível do RDY no fim do arranque/resume e inicializa o work/semáforo antes de armar a
interrupção. O RDY tem ainda pull-down no `corne_right.overlay`.

## Compilar e flashar

1. Cada push compila automaticamente em **Actions**. Descarrega o artefacto `firmware`.
2. Liga cada metade por USB, carrega duas vezes em reset → aparece a drive `NICENANO`.
3. Copia o `corne_left-….uf2` para a esquerda e o `corne_right-….uf2` para a direita.
4. Na primeira vez (ou se as metades não emparelharem), flasha primeiro `settings_reset` nas duas.

## Afinações comuns

- **Eixos trocados / invertidos** – em `config/corne_right.overlay` descomenta
  `switch-xy`, `invert-x`, `invert-y` ou `invert-scroll-y` conforme a orientação do TPS65 no case.
- **Velocidade do cursor** – `sensitivity` em `corne_right.overlay`, ou um `&zip_xy_scaler` no
  `&tps65_listener` em `config/corne_left.overlay`.
- **Diagnóstico** – grava o `corne_right_usb_logging` na direita e abre a porta COM do teclado
  (115200 baud, ex. PuTTY) para ver as mensagens do driver `tps43`.
- **Trackpad na esquerda** – ver no histórico do git o commit "Move TPS65 trackpad to left (central) half".
- **Mais gestos** – o driver suporta ainda `swipes`, `zoom` e `three-finger-tap`
  (ver o [README do driver](https://github.com/essenceotd/zmk_driver_azoteq)).
- **RGB underglow** – descomenta as linhas em `config/corne.conf`.
