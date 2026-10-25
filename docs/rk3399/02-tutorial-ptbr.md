# Tutorial (PT-BR) — instalar o RX3 numa Orange Pi 4 LTS ou Orange Pi 3 LTS

Passo a passo para montar uma unidade nova **a partir do pacote pronto**
(`rx3-rk3399-bundle.tar.gz`), gerado numa placa que já funciona. Não precisa
compilar nada nem extrair firmware: o pacote já traz o projeto inteiro, o
chroot com o `rbp`, os shims, o DirectFB modificado e o core KEY/STEMS.

Para montar do zero, sem pacote (extraindo o firmware e compilando tudo), use o
tutorial em inglês, [01-tutorial.md](01-tutorial.md).

> **Situação (2026-10-25):** instalação completa **testada numa Orange Pi 3
> LTS** limpa, com o pacote enviado pela rede (§4.1). Funcionaram tela, touch, menu
> de cima, DDJ-400, som e os dois pendrives (USB1/USB2). Na OPi3 a waveform roda
> a **30 fps** (limite do processador, veja a tabela). A instalação do zero numa
> Orange Pi 4 LTS nova ainda não foi repetida com este pacote; a OPi4 de
> referência roda o mesmo código.

> **Aviso:** o pacote contém o firmware extraído do XDJ-RX3. Use só nas suas
> próprias unidades e guarde o arquivo em mídia particular (HD/pendrive), nunca
> no GitHub. Veja [NOTICE.md](../../NOTICE.md).

---

## 0. O que você precisa

| Item | Observação |
|---|---|
| **Orange Pi 4 LTS** (RK3399, 4 GB) **ou Orange Pi 3 LTS** (Allwinner H6, 2 GB, eMMC 8 GB) | |
| Cartão microSD de 8 GB ou mais | para gravar o sistema |
| Fonte de alimentação boa | OPi4: 5 V / 4 A (USB-C). OPi3 LTS: 5 V / 3 A (conector redondo). Fonte fraca causa travamentos e falhas de USB |
| Dissipador (de preferência com ventoinha) | o player roda com a CPU no máximo o tempo todo |
| Monitor HDMI + touch USB (ou mouse USB) | touch testado: `wch.cn TouchScreen` |
| **Pioneer DDJ-400** | de preferência **direto** na placa. Na OPi3 funcionou num hub alimentado |
| HD externo ou pendrive com o pacote | a pasta `rx3-bundle-AAAAMMDD-HHMM/` inteira |
| Pendrive com músicas exportadas pelo rekordbox | precisa ter `PIONEER/rekordbox/export.pdb` |
| PC com o [balenaEtcher](https://etcher.balena.io/) | para gravar o cartão |
| Acesso à rede (cabo ou Wi-Fi) | para o `apt` e para acessar por SSH |

### Diferenças entre as placas

| | Orange Pi 4 LTS | Orange Pi 3 LTS |
|---|---|---|
| Processador | RK3399: 2× A72 1,8 GHz + 4× A53 1,4 GHz | H6: 4× A53 1,6 GHz |
| RAM | 4 GB | 2 GB (o `rbp` usa ~340 MB, sobra bastante) |
| Armazenamento | cartão SD / eMMC | eMMC 8 GB (+ cartão SD) |
| Kernel | Armbian **current** (6.18) | Armbian **legacy** (6.12): o current e o edge não rodam programas de 32 bits (§2.2) |
| Vídeo | `rockchip-drm` | `sun4i-drm`, com `video=HDMI-A-1:1920x1080@60` no boot (o instalador coloca) |
| Waveform | 60 fps | **30 fps** estáveis: o A53 gasta ~19–29 ms por quadro |
| Núcleos | interface nos A72 (4,5) | **2–3 só para o áudio**, 0–1 para a waveform e o resto (o instalador configura) |
| USB1 / USB2 | qualquer porta → USB1 | **porta azul (USB 3.0) = USB1**, **porta USB 2.0 livre = USB2** |
| Som da placa (HDMI/analógico) | desligado | desligado: só a DDJ-400 toca |

Na OPi3, o som fica limpo porque o áudio tem dois núcleos exclusivos e roda em
tempo real. A waveform fica em 30 fps estáveis porque o `rbp` desenha tudo em
software e um A53 não fecha um quadro em 16,7 ms. Testado na saída de 16 bits:
não ganha nada.

---

## 1. Baixar e gravar o sistema

1. Na página da sua placa em **armbian.com/download**:
   * Orange Pi 4 LTS → <https://www.armbian.com/orange-pi-4-lts/>
   * Orange Pi 3 LTS → <https://www.armbian.com/orange-pi-3-lts/>
2. Escolha a imagem **Debian 13 (trixie)**, kernel **current**, versão
   **minimal / server (CLI)**, sem desktop. É a mesma base da unidade de
   referência: Armbian 26.8.3, Debian 13, kernel 6.18. Na **OPi3 LTS** o kernel
   é trocado depois para o legacy (§2.2).
3. Grave o arquivo `.img.xz` no cartão microSD com o balenaEtcher. Não precisa
   descompactar antes.
4. Coloque o cartão na placa, ligue o cabo de rede e a fonte.

---

## 2. Primeiro boot

Entre pelo monitor + teclado, ou por SSH (`ssh root@<ip-da-placa>`; o IP
aparece no seu roteador).

1. Login inicial: usuário `root`, senha `1234`. O Armbian pede uma senha nova
   para o root.
2. Quando ele pedir para **criar um usuário normal, crie `orangepi`**. Esse é o
   nome que o serviço usa; outro nome funciona com a opção `--usuario`, mas
   `orangepi` evita dor de cabeça.
3. Escolha fuso horário e idioma quando ele perguntar
   (`America/Sao_Paulo`).
4. Atualize o sistema:

   ```sh
   sudo apt-get update && sudo apt-get upgrade -y
   sudo reboot
   ```

5. Wi-Fi (se não usar cabo): `sudo nmtui` ou `sudo armbian-config` →
   Network.

### 2.1 Só na Orange Pi 3 LTS: passar o sistema para o eMMC (opcional)

Depois de ver que tudo funciona pelo cartão, dá para mover o sistema para o eMMC
interno de 8 GB:

```sh
sudo armbian-install
```

Escolha **"Boot from eMMC — system on eMMC"**. Quando terminar, desligue, tire o
cartão e ligue de novo. Faça isso **antes** do passo 4: assim o pacote é
instalado direto no eMMC.

Espaço no eMMC: o sistema mínimo ocupa ~1,3 GB e o projeto ~2,8 GB depois de
instalado. O pacote (2,1 GB) é lido **direto do HD ou pela rede**, sem ser
copiado para a placa antes.

### 2.2 Só na Orange Pi 3 LTS: kernel legacy (obrigatório)

O `rbp` é um programa ARM de **32 bits**. Os kernels Armbian **current** e
**edge** do H6 vêm sem esse suporte (`CONFIG_COMPAT` desligado); o **legacy**
tem, além do Wi-Fi, vídeo e áudio USB da placa. Sem ele o instalador para no
passo 1 e mostra o comando.

```sh
sudo apt-get install -y linux-image-legacy-sunxi64 linux-dtb-legacy-sunxi64
sudo apt-mark hold linux-image-current-sunxi64 linux-dtb-current-sunxi64
sudo reboot
uname -r        # depois do boot: 6.12.x-legacy-sunxi64
```

O `apt-mark hold` impede que um `apt upgrade` volte o boot para o kernel
current.

> **Cuidado com `update-initramfs`** nessa placa: use sempre
> `sudo update-initramfs -u -k "$(uname -r)"`. Sem o `-k`, ele gera o initramfs
> do kernel mais novo (o current) e o Armbian passa a apontar o boot para ele.

---

## 3. Ligar o HD com o pacote

Ligue o HD externo (ou o pendrive com o pacote) numa porta USB e monte:

```sh
lsblk -f                      # ache a partição do HD (ex.: sda1, rótulo AZ-DATA)
sudo mkdir -p /mnt/hd
sudo mount /dev/sda1 /mnt/hd  # troque sda1 pelo que o lsblk mostrou
ls /mnt/hd/rx3-bundle/        # deve listar a pasta rx3-bundle-AAAAMMDD-HHMM
```

---

## 4. Rodar o instalador

```sh
cd /mnt/hd/rx3-bundle/rx3-bundle-AAAAMMDD-HHMM    # a pasta mais recente
cat MANIFEST.txt                                  # de onde e quando o pacote veio
sudo sh install-bundle.sh rx3-rk3399-bundle.tar.gz
```

### 4.1 Instalar pela rede, a partir da placa que tem o pacote

Sem levar o HD até a placa nova: com a chave SSH do root instalada nela, rode
**na placa onde está o pacote**:

```sh
P=/mnt/az-data/rx3-bundle/rx3-bundle-AAAAMMDD-HHMM
scp $P/install-bundle.sh root@<ip-da-placa-nova>:/root/
ssh root@<ip-da-placa-nova> 'sh /root/install-bundle.sh -' < $P/rx3-rk3399-bundle.tar.gz
```

O `-` faz o instalador ler o pacote da rede, sem gravar o arquivo de 2,1 GB na
placa. Pelo Wi-Fi leva ~20 minutos. Confira o sha256 do pacote antes, na
origem: `cd $P && sha256sum -c SHA256SUMS`.

### 4.2 O que o instalador faz

O instalador mostra cada etapa (`== 1/8 ...` até `== 8/8 ...`):

1. **verificações**: root, arm64, placa reconhecida (RK3399 ou H6), usuário
   `orangepi` e espaço livre. Confere também o **sha256** do pacote, para pegar
   arquivo corrompido;
2. **pacotes**: `alsa-utils`, `gcc`, `make`, `python3` etc.;
3. **extração** para `/home/orangepi/RX3-RBPI4B-RK3399` (alguns minutos);
4. **teste do chroot**: confirma que o kernel roda programas ARM de 32 bits;
5. **compila o `ddj400-bridge`** nesta placa;
6. adiciona **`rt_group_sched=0`** em `/boot/armbianEnv.txt`, só nos kernels que
   têm esse parâmetro (OPi4). Deixa um backup `.bak-rx3-...` ao lado;
7. **ajustes da placa**: cria
   `/etc/systemd/system/rx3-rbpi4b-rk3399.service.d/board.conf`, com a placa de
   vídeo certa. Na **OPi3** também:
   * `video=HDMI-A-1:1920x1080@60` no boot. Sem ele o H6 sobe em 1024×768 e não
     consegue mais mudar para o modo do player ("out of memory" no `rbp.log`);
   * USB1 = porta azul, USB2 = porta USB 2.0 livre;
   * núcleos 2–3 só para o áudio, 0–1 para a waveform e o resto do sistema, e o
     áudio em tempo real (`RX3_AUDIO_CPUS`, `RX3_UI_CPUS`, `RX3_RT_UNLIMITED`);
   * desliga o som HDMI e o analógico da placa;
8. **instala e liga os serviços**: o player (`rx3-rbpi4b-rk3399`), o gesto
   liga/desliga da DDJ-400 e o atalho do teclado (`rx3-key-toggle`).

Opções úteis:

| Opção | Quando usar |
|---|---|
| `--substituir` | já existe uma instalação; a antiga vai para `RX3-RBPI4B-RK3399.old-<data>` |
| `--usuario NOME` | o usuário não é `orangepi` |
| `--sem-apt` | sem internet (os pacotes do passo 2 já precisam estar instalados) |

No final, **reinicie**:

```sh
sudo reboot
```

---

## 5. Ligar o equipamento e conferir

Com a placa desligada, ligue:

* o monitor HDMI e o touch USB;
* a **DDJ-400 direto numa porta USB da placa**;
* depois, a fonte.

Uns 20–30 segundos após o boot, a tela do XDJ-RX3 aparece.

Checklist:

- [ ] a tela de decks aparece no monitor;
- [ ] o touch responde (toque num botão);
- [ ] ao ligar o pendrive do rekordbox, ele aparece em **SOURCE → USB1**;
- [ ] PLAY/CUE, jog e pads da DDJ-400 funcionam;
- [ ] sai som na saída MASTER (RCA) e no fone.

**Ganho de áudio:** ajuste o **TRIM** de cada canal até o medidor acender o
laranja nos picos, e deixe o **MASTER LEVEL** no máximo. O ganho digital já
vem ajustado para esse ponto.

Comandos para ver o que está acontecendo (por SSH):

```sh
systemctl status rx3-rbpi4b-rk3399
tail -f ~/RX3-RBPI4B-RK3399/work/log/launcher-run.log
tail -f ~/RX3-RBPI4B-RK3399/work/log/rbp.log
sudo sh ~/RX3-RBPI4B-RK3399/scripts/rk3399/start-rk3399.sh status
```

**Atalho no teclado:** segurar **DEL por 5 segundos** fecha o player (o console
de texto volta) ou abre de novo. Funciona com qualquer teclado USB, inclusive
os ligados depois do boot.

**Pendrives:** ao retirar um pendrive, use antes **USB STOP** no menu de cima e
segure **HOLD USB 1/2** por 3 s. Só aquele slot é desmontado e o outro deck
continua tocando. Se o pendrive da faixa que está tocando sair, o deck entra
no **loop de emergência** (comportamento do próprio player) e continua tocando.

**Achar a placa na rede:** o IPv4 muda a cada boot (DHCP). O IPv6 global não
muda, porque é gerado do MAC (`ip -6 -br a show wlan0`). Outra opção: reservar
um IP para o MAC no roteador.

Parar, iniciar ou reiniciar o player:

```sh
sudo systemctl stop rx3-rbpi4b-rk3399
sudo systemctl start rx3-rbpi4b-rk3399
sudo systemctl restart rx3-rbpi4b-rk3399
```

---

## 6. Problemas comuns

| Sintoma | O que fazer |
|---|---|
| Instalador para em **"nao roda programas de 32 bits"** / **"o chroot nao executa"** | OPi3: instale o kernel **legacy** (§2.2). OPi4: use a imagem Armbian **oficial "current"** |
| **"usuario 'orangepi' nao existe"** | crie com `sudo adduser orangepi && sudo usermod -aG sudo orangepi`, ou use `--usuario` |
| **"espaco insuficiente"** | OPi3: confira se o sistema está mesmo no eMMC (`df -h /`) e que o pacote está sendo lido do HD, não de uma cópia na placa |
| `rbp.log` com **"out of memory"** e nada na tela (OPi3) | falta `video=HDMI-A-1:1920x1080@60` em `/boot/armbianEnv.txt` (linha `extraargs=`); reinicie |
| Menu de cima não aparece ao arrastar (OPi3) | shim antigo: o painel ficava atrás da imagem. Use um pacote de 2026-10-25 ou mais novo |
| Pendrive na porta azul não é reconhecido (OPi3) | o controlador USB 3.0 do H6 pode travar quando se tira um hub dela com a placa ligada ("HC died" no `dmesg`). O watcher religa sozinho em ~5 s; se não voltar, reinicie |
| Tirar um pendrive deixou o outro deck mudo, com a waveform disparada | eject nativo antigo. Use um pacote de 2026-10-25 ou mais novo (o USB STOP agora ejeta só aquele slot) e reinicie o player |
| Waveform a 30 fps na OPi3 | normal nessa placa (limite do A53). Na OPi4 são 60 fps |
| Som saindo pelo monitor | som HDMI da placa. O instalador desliga (`/etc/modprobe.d/rx3-no-onboard-audio.conf`) |
| Tela preta / imagem cortada | teste o vídeo pelo caminho antigo, mais compatível: `sudo systemctl edit rx3-rbpi4b-rk3399` e adicione `[Service]` + `Environment=RB_KMS_SCANOUT=0`, salve e `sudo systemctl restart rx3-rbpi4b-rk3399`. Se mesmo assim não funcionar, tente também `Environment=RB_KMS_MODE=` |
| Sem som | `aplay -l` precisa listar `DDJ400`. Se não listar: cabo, porta USB, ligue a DDJ-400 direto na placa e reinicie |
| Cliques no som | OPi4: `cat /proc/cmdline` precisa ter `rt_group_sched=0` (reinicie depois de instalar). Os dois: `grep JuceALSA ~/RX3-RBPI4B-RK3399/work/log/launcher-run.log` deve dizer `SCHED_FIFO 45`. Na OPi3, veja o aquecimento (`cat /sys/class/thermal/thermal_zone0/temp`): a partir de 75000 (75 °C) ela começa a reduzir a velocidade, e um cooler resolve |
| O pendrive não aparece | precisa ter exportação do rekordbox (`PIONEER/rekordbox/export.pdb`). Log: `~/RX3-RBPI4B-RK3399/work/log/` e `docs/10-usb.md` |
| O serviço reinicia sem parar | `journalctl -u rx3-rbpi4b-rk3399 -b` e o fim de `work/log/rbp.log` |

---

## 7. Gerar um pacote novo (na placa que já funciona)

Depois de mudar algo na unidade de referência, gere um pacote atualizado:

```sh
sudo mount /dev/sdX1 /mnt/hd        # se o HD ainda não estiver montado
sudo sh ~/RX3-RBPI4B-RK3399/scripts/rk3399/bundle/make-bundle.sh /mnt/hd/rx3-bundle
```

Ele cria `/mnt/hd/rx3-bundle/rx3-bundle-AAAAMMDD-HHMM/` com:

| Arquivo | Conteúdo |
|---|---|
| `rx3-rk3399-bundle.tar.gz` | o projeto inteiro (repositório + `work/`: chroot, DirectFB, toolkit, bridge) |
| `install-bundle.sh` | o instalador |
| `MANIFEST.txt` | placa de origem, commit, número de BUILD, tamanhos |
| `SHA256SUMS` | para conferir se o arquivo não corrompeu |

Dá para gerar com o player rodando: o script lê os arquivos por uma montagem
separada da raiz e não pega o pendrive nem as montagens internas do chroot.

---

## 8. Backup completo e restauração da Orange Pi 4 de referência

Além do pacote, o HD guarda uma cópia do **sistema inteiro** da OPi4, em
`rx3-backup/full-AAAAMMDD-HHMM/`:

| Arquivo | Conteúdo |
|---|---|
| `rootfs-orangepi4.tar.gz` (+ `.sha256`) | todo o sistema de arquivos raiz (`/`) |
| `mmcblk1-first16M-bootloader.img.gz` | os primeiros 16 MB do eMMC: tabela de partições e bootloader |
| `system-info.txt` | partições, UUIDs, kernel, versão do sistema |
| `dpkg-selections.txt`, `apt-manual.txt` | lista de pacotes instalados |

Restauração (último recurso, **não testada**): grave o Armbian da OPi4 num
cartão SD e inicie por ele. Depois, com o eMMC de destino como `/dev/mmcblk1`
(**confira com `lsblk`: o comando abaixo apaga esse disco**):

```sh
gunzip -c mmcblk1-first16M-bootloader.img.gz | sudo dd of=/dev/mmcblk1 bs=1M
sudo partprobe /dev/mmcblk1
sudo mkfs.ext4 -U 154ac854-7ab5-42c7-b8a3-8a214e30841a /dev/mmcblk1p1
sudo mkdir -p /mnt/alvo
sudo mount /dev/mmcblk1p1 /mnt/alvo
sudo tar -xzpf rootfs-orangepi4.tar.gz --numeric-owner -C /mnt/alvo
sudo umount /mnt/alvo
```

O UUID do `mkfs` é o que está em `system-info.txt`: ele precisa bater com
`rootdev=` do `/boot/armbianEnv.txt` e com o `/etc/fstab` da cópia. O disco de
destino precisa ter pelo menos o tamanho do original (15 GB).

Para uma unidade nova, o caminho normal é o pacote (passos 1–5). A restauração
completa serve para reconstruir a unidade de referência exatamente como estava.
