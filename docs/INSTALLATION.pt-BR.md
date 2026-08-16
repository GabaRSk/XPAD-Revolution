# Instalação

## 1. Instale o plugin no PS3

Faça backup do plugin que já funciona. Copie estes arquivos do ZIP para `/dev_hdd0/plugins/ps3xpad/`:

```text
xpad_vsh_autodiscovery.sprx
xpad_game.sprx
xpad_devices.txt
xpad_remap.txt
xpad_analog.txt
```

Adicione somente esta linha a `/dev_hdd0/boot_plugins.txt`:

```text
/dev_hdd0/plugins/ps3xpad/xpad_vsh_autodiscovery.sprx
```

`xpad_vsh_autodiscovery.sprx` é o módulo atrasado e seguro para o XMB no HEN. `xpad_game.sprx` possui outra assinatura, destinada ao processo do jogo, e deve ficar fora do `boot_plugins.txt`.

Reinicie, ative o HEN e aguarde cerca de dez segundos. A mensagem deve mostrar `XPAD Rev v1.0.0 Loaded!`.

## 2. Carregue o módulo de jogo sem PC

Depois de chegar ao menu do jogo, segure **SELECT + L3 + R3** por aproximadamente 0,8 segundo. No DS4, SELECT é SHARE; no DualSense, CREATE. O módulo VSH encontra o EBOOT ativo e carrega `xpad_game.sprx`. O sucesso mostra:

```text
XPAD Rev: game module active
```

O atalho é liberado novamente quando o jogo fecha. Normalmente não é necessário PID fixo, comando pelo navegador ou PC.

## 3. Instale o viewer no Windows

1. Execute `Instalar_XPAD_Revolution.exe` como administrador.
2. Deixe marcada a opção de controle virtual Xbox 360 se quiser usar viewers comuns do navegador.
3. Confirme o instalador oficial do ViGEmBus quando o Windows exibi-lo.
4. Inicie o XPAD Revolution e aguarde a detecção do PS3.

Não é necessário configurar IP fixo. O aplicativo anuncia o PC pela UDP 39001 e recebe a telemetria pela UDP 39000. O instalador cria a regra necessária no Firewall do Windows somente para perfil privado e rede local.

Viewer local: `http://127.0.0.1:8765/?pad=0`

Acrescente `&debug=1` para mostrar os valores numéricos dos gatilhos.

O instalador v1.0.0 encerra o viewer antigo e remove seus atalhos, regra de firewall e registro de desinstalação. A pasta antiga é mantida de propósito para não apagar automaticamente arquivos do usuário.

## Recuperação

Se o módulo VSH causar problema, restaure o SPRX anterior pelo seu método de recuperação/FTP. Se apenas um jogo apresentar problema, reinicie-o e não acione o atalho; `xpad_game.sprx` não é carregado no boot.
