#!/bin/sh
# install-bundle.sh -- install a make-bundle.sh tarball on a fresh Armbian
# (Debian 13 trixie, aarch64): Orange Pi 4 LTS (RK3399) or Orange Pi 3 LTS (H6).
#
#   sudo sh install-bundle.sh [opcoes] rx3-rk3399-bundle.tar.gz
#   ssh root@placa 'sh install-bundle.sh -' < rx3-rk3399-bundle.tar.gz
#
#   "-" reads the tarball from stdin: over the network, nothing is stored on the
#   board first (the 8 GB eMMC of an Orange Pi 3 LTS cannot hold the tarball and
#   the install at once). No sha256 check then: verify the file at the source.
#
#   --usuario NOME   owner of the install (default orangepi; must already exist)
#   --sem-apt        do not apt-get install the host packages
#   --substituir     an existing install is moved aside to <dir>.old-<date>
#   --forcar         continue on a board that is neither RK3399 nor H6
#
# What it does, in order: checks (root, aarch64, board, space) -> host packages
# -> untar into ~user/RX3-RBPI4B-RK3399 -> proves the 32-bit chroot executes ->
# rebuilds ddj400-bridge natively -> rt_group_sched=0 on the kernel command line
# -> a per-board systemd drop-in (DRM card, CPU pinning) -> install-service.sh.
# Reboot afterwards: the kernel argument only takes effect on the next boot.
#
# Run it from the USB drive the bundle is on, or stream it with "-": either way
# the tarball is never copied to the board's own storage.
set -eu

USER_NAME=orangepi APT=1 REPLACE=0 FORCE=0 BUNDLE=
while [ $# -gt 0 ]; do
    case "$1" in
        --usuario) USER_NAME=$2; shift 2 ;;
        --sem-apt) APT=0; shift ;;
        --substituir) REPLACE=1; shift ;;
        --forcar) FORCE=1; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        -) BUNDLE=-; shift ;;
        -*) echo "opcao desconhecida: $1" >&2; exit 2 ;;
        *) BUNDLE=$1; shift ;;
    esac
done

say()  { printf '\n== %s\n' "$*"; }
die()  { printf 'ERRO: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "rode como root (sudo)"
[ -n "$BUNDLE" ] || die "uso: sudo sh install-bundle.sh [opcoes] rx3-rk3399-bundle.tar.gz"
[ "$BUNDLE" = - ] || [ -f "$BUNDLE" ] || die "pacote nao encontrado: $BUNDLE"
[ "$(uname -m)" = aarch64 ] || die "precisa de um sistema aarch64 (arm64), este e $(uname -m)"

say "1/8 verificacoes"
COMPAT=$(tr '\0' ' ' < /proc/device-tree/compatible 2>/dev/null || echo '')
case "$COMPAT" in
    *rockchip,rk3399*)    BOARD=rk3399 ;;
    *allwinner,sun50i-h6*) BOARD=h6 ;;
    *) BOARD=other
       [ "$FORCE" -eq 1 ] || die "placa nao reconhecida ($COMPAT); use --forcar para tentar mesmo assim" ;;
esac
echo "placa: $BOARD ($COMPAT)"

# rbp is a 32-bit ARM binary: the kernel must run AArch32 userland. Armbian's
# sunxi64 current/edge kernels ship without it (2026-10); legacy has it.
KCONF=/boot/config-$(uname -r)
if [ -r "$KCONF" ] && ! grep -q '^CONFIG_COMPAT=y' "$KCONF"; then
    [ "$BOARD" = h6 ] && die "o kernel $(uname -r) nao roda programas de 32 bits (CONFIG_COMPAT). Na Orange Pi 3 LTS instale o kernel legacy: apt-get install -y linux-image-legacy-sunxi64 linux-dtb-legacy-sunxi64 && reboot"
    die "o kernel $(uname -r) nao roda programas de 32 bits (CONFIG_COMPAT desligado)"
fi

getent passwd "$USER_NAME" >/dev/null ||
    die "usuario '$USER_NAME' nao existe (crie no primeiro login do Armbian, ou use --usuario)"
HOME_DIR=$(getent passwd "$USER_NAME" | cut -d: -f6)
UID_N=$(id -u "$USER_NAME"); GID_N=$(id -g "$USER_NAME")
DEST="$HOME_DIR/RX3-RBPI4B-RK3399"
echo "destino: $DEST"

# A sha256 next to the bundle is checked when present.
SUMS="$(dirname "$BUNDLE")/SHA256SUMS"
if [ "$BUNDLE" != - ] && [ -f "$SUMS" ]; then
    want=$(awk '$2=="rx3-rk3399-bundle.tar.gz"{print $1}' "$SUMS")
    if [ -n "$want" ]; then
        echo "conferindo sha256 do pacote (leva alguns minutos)..."
        got=$(sha256sum "$BUNDLE" | cut -d' ' -f1)
        [ "$got" = "$want" ] || die "sha256 nao confere: pacote corrompido"
        echo "sha256 ok"
    fi
fi

# Unpacked ~ 1.4x the gzip -1 tarball (2.1 GB -> 2.8 GB), plus 500 MB margin
# for packages and logs. From stdin the size is unknown: RX3_NEED_MB, default
# sized for the 2026-10 bundle.
if [ "$BUNDLE" = - ]; then
    need_kb=$(( ${RX3_NEED_MB:-3400} * 1024 ))
else
    need_kb=$(( $(du -k "$BUNDLE" | cut -f1) * 14 / 10 + 512000 ))
fi
have_kb=$(df -Pk "$HOME_DIR" | awk 'NR==2{print $4}')
echo "espaco: precisa ~$((need_kb/1024)) MB, livre $((have_kb/1024)) MB"
[ "$have_kb" -ge "$need_kb" ] || die "espaco insuficiente em $HOME_DIR"

if [ -e "$DEST" ]; then
    [ "$REPLACE" -eq 1 ] || die "$DEST ja existe (use --substituir para mover o antigo de lado)"
    systemctl stop rx3-rbpi4b-rk3399.service 2>/dev/null || true
    sh "$DEST/scripts/rk3399/start-rk3399.sh" stop 2>/dev/null || true
    grep -q " $DEST/" /proc/mounts && die "ainda ha montagens sob $DEST (veja: findmnt -R $DEST)"
    OLD="$DEST.old-$(date +%Y%m%d-%H%M)"
    mv "$DEST" "$OLD"
    echo "instalacao anterior movida para $OLD"
fi

say "2/8 pacotes do sistema"
if [ "$APT" -eq 1 ]; then
    # </dev/null: with "-" stdin is the tarball, and maintainer scripts read stdin.
    apt-get update </dev/null
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        alsa-utils util-linux procps psmisc gcc libc6-dev make python3 file </dev/null
else
    echo "pulado (--sem-apt)"
fi

say "3/8 extraindo o pacote (alguns minutos)"
if [ "$BUNDLE" = - ]; then
    tar -xzpf - --numeric-owner -C "$HOME_DIR"
else
    tar -xzpf "$BUNDLE" --numeric-owner -C "$HOME_DIR"
fi
[ -x "$DEST/scripts/rk3399/start-rk3399.sh" ] || [ -f "$DEST/scripts/rk3399/start-rk3399.sh" ] ||
    die "o pacote nao contem RX3-RBPI4B-RK3399/"
RT="$DEST/work/runtime/rx3"
# The bundle is made on a unit whose user is uid 1000; hand those files over.
if [ "$UID_N" != 1000 ] || [ "$GID_N" != 1000 ]; then
    chown -R --from=1000:1000 "$UID_N:$GID_N" "$DEST"
fi
# Excluded by make-bundle.sh (rbp appends to it without bound).
[ -e "$RT/dev/gpiodrv" ] || { : > "$RT/dev/gpiodrv"; chmod 666 "$RT/dev/gpiodrv"; }
mkdir -p "$DEST/work/log"
chown "$UID_N:$GID_N" "$DEST/work/log"

say "4/8 testando o chroot ARM32"
if [ "$(chroot "$RT" /bin/sh -c 'echo ok' 2>/dev/null)" = ok ]; then
    echo "chroot executa binarios de 32 bits: ok"
else
    die "o chroot nao executa: o kernel precisa de CONFIG_COMPAT (suporte a ARM 32 bits). Use o Armbian 'current' oficial da placa."
fi

say "5/8 ddj400-bridge (compilado nesta placa)"
if command -v gcc >/dev/null 2>&1; then
    su "$USER_NAME" -c "sh '$DEST/scripts/rk3399/build-host-tools.sh'" ||
        echo "aviso: falhou ao compilar; usando o binario do pacote" >&2
fi
[ -x "$DEST/work/build/host/ddj400-bridge" ] || die "work/build/host/ddj400-bridge ausente"

say "6/8 parametro do kernel rt_group_sched=0"
ENV=/boot/armbianEnv.txt
# The rt_group_sched= boot parameter exists only in kernels that have
# CONFIG_RT_GROUP_SCHED_DEFAULT_DISABLED (6.16+): the RK3399's 6.18, not the H6
# legacy 6.12. Without it the launcher just reports JuceALSA left SCHED_OTHER.
if [ -r "$KCONF" ] && ! grep -q 'RT_GROUP_SCHED_DEFAULT_DISABLED' "$KCONF"; then
    echo "kernel $(uname -r) nao tem esse parametro: pulado"
elif [ -f "$ENV" ]; then
    if grep -q 'rt_group_sched=0' "$ENV"; then
        echo "ja presente"
    else
        cp -p "$ENV" "$ENV.bak-rx3-$(date +%Y%m%d-%H%M)"
        if grep -q '^extraargs=' "$ENV"; then
            sed -i 's/^extraargs=\(.*\)$/extraargs=\1 rt_group_sched=0/' "$ENV"
        else
            echo 'extraargs=rt_group_sched=0' >> "$ENV"
        fi
        echo "adicionado em $ENV (backup ao lado)"
    fi
else
    echo "aviso: $ENV nao existe; adicione rt_group_sched=0 na linha de comando do kernel manualmente" >&2
fi

say "7/8 ajustes da placa ($BOARD)"
# H6: unless the console starts in a big mode, sun4i-drm comes up at 1024x768,
# takes pll-video0 exclusively at 195 MHz and then rejects every mode above
# 65 MHz -- fb0 stays 1024x768, too small for rbp's 1280x800 (DirectFB "out of
# memory" in dfb_layer_context_init). Starting at 1080p leaves pll-video0 at
# 297 MHz, from which the shim's 1280x800 @ 74.25 MHz is reachable. Verified on
# an Orange Pi 3 LTS, legacy 6.12, 2026-10-25.
if [ "$BOARD" = h6 ] && [ -f "$ENV" ] && ! grep -q 'video=HDMI-A-1:' "$ENV"; then
    cp -p "$ENV" "$ENV.bak-rx3-video-$(date +%Y%m%d-%H%M)"
    if grep -q '^extraargs=' "$ENV"; then
        sed -i 's/^extraargs=\(.*\)$/extraargs=\1 video=HDMI-A-1:1920x1080@60/' "$ENV"
    else
        echo 'extraargs=video=HDMI-A-1:1920x1080@60' >> "$ENV"
    fi
    echo "video=HDMI-A-1:1920x1080@60 adicionado em $ENV (precisa reiniciar)"
fi
# The DRM card that drives HDMI (rockchip-drm card0 on the RK3399; on the H6
# whichever card carries the HDMI connector -- the GPU's card has none).
CARD=
for c in /sys/class/drm/card*-HDMI-A-*; do
    [ -e "$c" ] || continue
    c=${c##*/}; CARD=/dev/dri/${c%%-*}; break
done
[ -n "$CARD" ] || { CARD=/dev/dri/card0; echo "aviso: conector HDMI nao achado; usando $CARD" >&2; }
[ -e /dev/fb0 ] || echo "aviso: /dev/fb0 nao existe (fbdev do DRM desligado?) -- o rbp precisa dele" >&2
D=/etc/systemd/system/rx3-rbpi4b-rk3399.service.d
mkdir -p "$D"
{
    echo "# written by install-bundle.sh for board '$BOARD'"
    echo "[Service]"
    echo "Environment=RX3_DRI_CARD=$CARD"
    # The launcher pins gui_task to cpus 4,5 (the RK3399's Cortex-A72s). The H6
    # has four identical A53s (0-3) and no cpu 4/5: leave it unpinned.
    [ "$BOARD" = rk3399 ] || echo "Environment=RX3_GUI_CPUS="
    # H6: rbp's USB1/USB2 by physical port (usb-watch.sh, two-slot mode).
    # Orange Pi 3 LTS: USB 3.0 port = 5200000.usb; the USB 2.0 port on
    # EHCI 5311000 (+ OHCI companion 5311400) = USB2 (set up 2026-10-25).
    if [ "$BOARD" = h6 ]; then
        echo "Environment=USBWATCH_PORT1=5200000.usb/"
        echo 'Environment="USBWATCH_PORT2=5311000.usb/ 5311400.usb/"'
        # Four equal A53s: cores 2-3 only for the audio path, 0-1 for the
        # waveform/UI and the rest of the system (scripts/rk3399/cpu-split.sh),
        # and JuceALSA's SCHED_FIFO allowed on a kernel without rt_group_sched=
        # (start-rk3399.sh, RX3_RT_UNLIMITED). Set up 2026-10-25.
        echo "Environment=RX3_AUDIO_CPUS=2-3"
        echo "Environment=RX3_UI_CPUS=0-1"
        echo "Environment=RX3_RT_UNLIMITED=1"
    fi
} > "$D/board.conf"
cp "$DEST/scripts/rk3399/systemd/rx3-rbpi4b-rk3399.service.d/oom-protect.conf" "$D/"
# H6: sound only through the DDJ-400. The board's HDMI audio (heard on the
# monitor at boot, 2026-10-25) and AC200 analog codec are never loaded. The
# initramfs is rebuilt for the RUNNING kernel only: a bare -u picks the newest
# installed one (6.18 current, kept on hold) and Armbian then points
# /boot/uInitrd at it -- a mismatch with the legacy kernel that boots.
if [ "$BOARD" = h6 ]; then
    cat > /etc/modprobe.d/rx3-no-onboard-audio.conf <<'EOF2'
# RX3 (install-bundle.sh): sound only through the DDJ-400 (snd_usb_audio).
blacklist sun9i_hdmi_audio
blacklist dw_hdmi_i2s_audio
blacklist snd_soc_hdmi_codec
blacklist snd_soc_ac200
blacklist snd_soc_simple_card
install sun9i_hdmi_audio /bin/false
install dw_hdmi_i2s_audio /bin/false
install snd_soc_hdmi_codec /bin/false
install snd_soc_ac200 /bin/false
install snd_soc_simple_card /bin/false
EOF2
    update-initramfs -u -k "$(uname -r)" </dev/null >/dev/null 2>&1 ||
        echo "aviso: update-initramfs falhou" >&2
    echo "audio da placa (HDMI/analogico) desligado: so a DDJ-400 toca"
fi
cat "$D/board.conf"
# The units name /home/orangepi; follow a different home.
if [ "$HOME_DIR" != /home/orangepi ]; then
    sed -i "s#/home/orangepi#$HOME_DIR#g" "$DEST"/scripts/rk3399/systemd/*.service \
        "$DEST"/scripts/rk3399/systemd/*.path
fi

say "8/8 servico systemd"
sh "$DEST/scripts/rk3399/install-service.sh"

say "pronto"
echo "Reinicie agora (parametros do kernel so valem no proximo boot):  sudo reboot"
echo "Depois:  systemctl status rx3-rbpi4b-rk3399   e   tail -f $DEST/work/log/rbp.log"
