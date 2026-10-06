# zmk-config — Corne v3 (6 colunas) + trackpad Azoteq TPS65

Firmware [ZMK](https://zmk.dev) para um Corne v3 split de 6 colunas com um trackpad
**Azoteq TPS65** (controlador IQS550).

| Item        | Valor                                                                 |
| ----------- | --------------------------------------------------------------------- |
| MCU         | Pro Micro nRF52840 "V1940" (clone nice!nano v2) → board `nice_nano//zmk` |
| Central     | Metade **esquerda** (liga ao computador)                              |
| Periférico  | Metade **direita** com o trackpad (enviado à central via `zmk,input-split`) |
| Trackball   | Pimoroni PIM447 na esquerda (polegar), I2C 0x0A, INT em D9 — driver próprio em `drivers/input/pim447.c` |
| Ecrã        | OLED 128x32 (I2C 0x3C) com ecrãs personalizados (ver abaixo)          |
| LEDs        | 27 por metade (6 underglow + 21 por tecla), dados em D1 (P0.06)       |
| Keymap      | Editável ao vivo no [ZMK Studio](https://zmk.studio) (esquerda por USB) |
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
  corne_left.overlay       # trackball PIM447 + listeners (+ layer Mouse automática)
  corne_right.overlay      # trackpad no I2C + pinos RDY/NRST
  corne_common.dtsi        # partilhado: input-split + 27 LEDs
  corne.keymap             # layers Base / Lower / Raise / Mouse
drivers/ dts/ zephyr/      # drivers: IQS5xx (TPS65, com correção do RDY) e PIM447 (trackball)
src/display/               # ecrãs OLED personalizados (LVGL)
tools/                     # gerador de pixel-art e pré-visualizações
```

### Correção no driver

O RDY do IQS550 é lido por interrupção de flanco. Se o chip levantar o RDY antes de
a interrupção estar armada (arranque ou saída de suspensão), o flanco perde-se e o
trackpad fica parado até haver um pulso no pino. A cópia do driver nesta repo verifica
o nível do RDY no fim do arranque/resume e inicializa o work/semáforo antes de armar a
interrupção. O RDY tem ainda pull-down no `corne_right.overlay`.

### Arranque robusto

- **Ciclo de energia no arranque** (`src/power/boot_power_cycle.c`): em cada arranque o
  VCC dos periféricos (trackpad, trackball, OLED, LEDs) é desligado 300 ms, com os pinos
  I2C em baixo consumo para não os alimentar "por trás", e volta a ligar — um reset
  limpo, igual a tirar e voltar a pôr a bateria.
- **Novas tentativas**: se o trackpad ou o trackball não responderem no arranque, o
  driver liberta o barramento I2C e tenta outra vez a cada 2 s (até ~30 s), em vez de
  ficar parado até ao próximo reset.

## Trackball Pimoroni (esquerda)

| PIM447 | Pro Micro (esq.) | nRF52840 | Notas                     |
| ------ | ---------------- | -------- | ------------------------- |
| 3-5V   | VCC              | –        | 3.3 V                     |
| GND    | GND              | –        |                           |
| SDA    | D2               | P0.17    | partilhado com o OLED     |
| SCL    | D3               | P0.20    | partilhado com o OLED     |
| INT    | D9               | P1.06    | ativo em LOW (pull-up)    |

- **Clique da bola** alterna **cursor ⇄ scroll** (o LED pisca azul = cursor, verde = scroll;
  o OLED da esquerda mostra o modo ao lado das baterias).
- **LED da bola = parte do RGB do teclado**: é o 28.º LED da metade esquerda
  (`ball_and_strip` em `corne_left.overlay`), por isso segue efeitos, cor, brilho, on/off
  e o auto-off das teclas RGB da layer Lower.
- Afinações em `config/corne_left.overlay`: `cursor-multiplier`, `acceleration`,
  `scroll-divisor` e, para a orientação, `zip_xy_transform` no `trackball_listener`.
- Sem `click-toggles-scroll`, o clique passa a ser o botão esquerdo do rato.
- O driver usa o INT (com verificação de nível, para não perder flancos), apaga o LED
  e põe o PIM447 a dormir em deep sleep. Se o trackball não estiver ligado, o resto do
  teclado funciona normalmente.

## Ecrãs OLED

Pixel-art original, gerada por `tools/gen_art.py` (→ `src/display/art.c`).
`tools/mockup.py` gera uma pré-visualização aproximada em `tools/preview/`.

**Esquerda** — ícone animado da layer ativa (teclado / 123 / #! / cursor), nome da
layer, ligação (BT + perfil ou USB, ✓ ligado / ✗ desligado / ? livre), bateria das
duas metades e palavras por minuto.

**Direita** — o **Pixo**, uma mascote que reage: escreve quando carregas nas teclas da
direita, aponta quando usas o trackpad, arregala os olhos no scroll, adormece ao fim de
1 min e fica triste sem ligação à esquerda. Ao lado, um mini-mapa do trackpad (o ponto
segue o dedo; a moldura pisca nos cliques), modo atual, ligação e bateria.

## LEDs RGB

Teclas na layer **Lower**, fila de baixo à direita: `RGB on/off`, `efeito`, `cor`,
`saturação`, `brilho +`, `brilho −`. Os LEDs apagam-se quando o teclado fica inativo
e quando não há USB (poupar bateria). Brilho máximo limitado a 50 %.

> `CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER=n` é **obrigatório**: sem isso, desligar o RGB
> cortava o VCC e o trackpad/OLED deixavam de funcionar.

## ZMK Studio

1. Liga a metade **esquerda** por USB e abre <https://zmk.studio> (Chrome/Edge).
2. Para desbloquear: **Lower + tecla à direita do `;`** (`&studio_unlock`).
3. As alterações ficam guardadas no teclado; `config/corne.keymap` é a base.

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
