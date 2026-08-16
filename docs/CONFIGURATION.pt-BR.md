# Configuração

## Remapeamento

Edite `/dev_hdd0/plugins/ps3xpad/xpad_remap.txt` usando `BOTÃO_FÍSICO = BOTÃO_ENVIADO_AO_PS3`.

Exemplo de troca:

```text
R1 = R2
R2 = R1
```

`START_ENABLED = 1` ativa o perfil escolhido na inicialização. Se estiver em `0`, use **START + SELECT + DPAD RIGHT** para ativar ou desativar.

No DS4/DualSense conectado por USB, `TOUCHPAD = SELECT` ou `TOUCHPAD = START` transforma o clique do touchpad no botão desejado. O touchpad só pode ser uma origem. O Bluetooth nativo do PS3 remove esse bit antes que o XPAD Revolution o receba, portanto esse remapeamento exige USB.

O viewer mostra de propósito o comando físico antes do remapeamento; o PS3 recebe o resultado remapeado.

## Curva dos analógicos

Edite `/dev_hdd0/plugins/ps3xpad/xpad_analog.txt`:

```text
ANALOG_SATURATION = 80
ANALOG_DEADZONE = 4
ANALOG_GAME_MODE = AUTO
ANALOG_PORT_MASK = 1
```

- Saturação `80`: base geral próxima do DS3.
- `75`: alcança os cantos mais cedo.
- `85`: resposta mais suave.
- `100`: sem correção de saturação.
- Zona morta `4`: padrão conservador.
- Zona morta `0`: indicada para Hall effect sem drift.

`ANALOG_PORT_MASK` é uma máscara: `1` seleciona a porta 1, `2` a porta 2 e `3` as duas. Mantenha `ANALOG_GAME_MODE = AUTO`, exceto se um firmware ou adaptador específico classificar o controle incorretamente.

O módulo VSH corrige controles XInput compatíveis e DS4/DualSense por USB. O módulo de jogo corrige os controles nativos por Bluetooth selecionados depois de `cellPadGetData`.

## Logs

Log do VSH: `/dev_hdd0/plugins/ps3xpad/xpad.log`

Log do módulo de jogo: `/dev_hdd0/plugins/ps3xpad/xpad_game.log`
