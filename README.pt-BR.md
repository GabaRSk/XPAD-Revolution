# XPAD Revolution

[English](README.md) | [Português do Brasil](README.pt-BR.md)

XPAD Revolution é uma evolução comunitária do PS3xPAD para PS3HEN. A versão 1.0.0 reúne inicialização segura, amplo suporte a controles USB, baixa latência, remapeamento, correção dos analógicos e telemetria para Windows.

## Destaques

- Inicialização segura no PS3HEN com atraso de 10 segundos no módulo VSH.
- Leitura USB a 2 ms / 500 Hz com buffer seguro de 64 bytes.
- Descoberta dinâmica do endpoint interrupt-OUT dos controles compatíveis.
- Buffer híbrido de baixa latência que mantém o estado mais recente sem perder transições rápidas dos botões.
- Remapeamento seguro no HEN, inclusive clique do touchpad do DS4/DualSense como origem quando conectado por USB.
- Curva de saturação e zona morta radial configuráveis no estilo DualShock 3.
- Módulo de jogo unificado para correção dos analógicos, compatibilidade e vibração de DS4/DualSense nativos por Bluetooth.
- Carregador interno do módulo de jogo: segure **SELECT + L3 + R3 por 0,8 segundo** depois de chegar ao menu do jogo.
- Descoberta UDP automática, sem configurar IP fixo do PC ou do PS3.
- Viewer local com pressão real de L2/R2.
- Controle virtual Xbox 360 opcional no Windows para viewers de gamepad comuns.

Entre os exemplos compatíveis estão controles XInput no padrão Xbox 360, Flydigi Direwolf 2/4, controles Logitech, dongles compatíveis da 8BitDo/GameSir, DualShock 4, DualSense e DualSense Edge. Outros VID/PID USB podem ser adicionados ao `xpad_devices.txt`.

## Download

Baixe `XPAD-Revolution-v1.0.0.zip` na [versão mais recente](../../releases/latest). O ZIP contém os arquivos prontos para o PS3 e o instalador offline do viewer para Windows.

## Instalação rápida

1. Faça backup de `/dev_hdd0/plugins/ps3xpad/`.
2. Copie os cinco arquivos da pasta `plugin/ps3xpad/` do ZIP para `/dev_hdd0/plugins/ps3xpad/`.
3. Adicione somente esta linha a `/dev_hdd0/boot_plugins.txt`:

   ```text
   /dev_hdd0/plugins/ps3xpad/xpad_vsh_autodiscovery.sprx
   ```

4. **Não** coloque `xpad_game.sprx` no `boot_plugins.txt` e não renomeie um SPRX por cima do outro.
5. Reinicie, ative o HEN e aguarde `XPAD Rev v1.0.0 Loaded!`.
6. Dentro do jogo, segure **SELECT + L3 + R3 por 0,8 segundo** para carregar o módulo de jogo quando necessário.

Consulte [Instalação](docs/INSTALLATION.pt-BR.md) e [Configuração](docs/CONFIGURATION.pt-BR.md) para o guia completo.

## Limitação do Bluetooth nativo

O Bluetooth nativo do PS3 remove algumas informações específicas do DS4/DualSense antes que o plugin receba os dados. A correção dos analógicos e os hooks de compatibilidade funcionam, mas o clique do touchpad não pode ser recuperado e o viewer VSH não enxerga esse controle nativo. Use USB para remapear o touchpad e obter telemetria completa.

## Código-fonte e compilação

O repositório inclui o código modificado dos módulos do PS3 e do viewer para Windows. Por motivos legais, não inclui o Cell SDK da Sony, toolchain PPU, `scetool`, dados de assinatura ou objetos intermediários. Consulte [BUILDING.md](BUILDING.md).

## Créditos e licenças

XPAD Revolution deriva do **PS3xPAD de OsirisX**. Consulte [CREDITS.md](CREDITS.md) e [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

Use por sua conta e risco. Mantenha um backup funcional e um método de recuperação/FTP antes de substituir um plugin iniciado pelo boot.
