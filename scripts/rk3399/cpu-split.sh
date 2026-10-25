#!/bin/sh
# cpu-split.sh -- give the player's audio its own cores, everything else the rest.
#
#   sh cpu-split.sh apply <rbp-pid>   pin once: rbp's threads, the rest of the
#                                     system, the IRQs (the launcher calls it at
#                                     start and then every 2 s, since rbp makes
#                                     threads as it goes -- a PcmReader per load)
#   sh cpu-split.sh restore           everything back on every core (stop_all)
#
# Off unless both RX3_AUDIO_CPUS and RX3_UI_CPUS are set (e.g. 2-3 and 0-1 on the
# Orange Pi 3 LTS's four A53s, 2026-10-25, at the operator's request: "2 nucleos
# exclusivos para Audio e os outros 2 para a waveform").
#
# AUDIO = the whole audio path, not JuceALSA alone: on 2026-10-14 JuceALSA was
# pinned apart from the decoders and they starved -- constant clicks. So the
# set is JuceALSA (render + write to the DDJ-400), FGPR:* (decoders),
# PcmReader:* (file reads), JogCom:* (jogs), Player:*, WavWriter. Every other rbp
# thread -- gui_task (draws the waveforms, the biggest single load), Ui*, DB*,
# Tot_*, the shims' threads (comm "rbp") -- goes to UI.
#
# "Exclusive" is made, not assumed: every other userland task (PID 1 included,
# so services started later inherit it) is moved to UI, and so is every IRQ
# except the USB controller the DDJ-400 sits behind, which goes to AUDIO.
# Kernel threads are left alone (most are per-CPU and cannot move).
set -u

AUDIO=${RX3_AUDIO_CPUS:-}
UI=${RX3_UI_CPUS:-}
# gui_task draws and presents every frame and needs ~56% of an A53. Sharing the
# UI cores as plain SCHED_OTHER it waited 915 ms per 10 s on the run queue,
# preempted ~1500 times a second, and missed vblanks: the waveform stuttered
# (operator, 2026-10-25: "pequenos soquinhos, nao esta fluida em 60 hz") while
# the audio cores stayed clean. A low SCHED_FIFO priority -- under JuceALSA's 45,
# which is on other cores anyway -- lets it run the moment a frame is due.
# RX3_GUI_RT_PRIO= (empty) leaves it SCHED_OTHER.
GUI_RT=${RX3_GUI_RT_PRIO-20}
[ -n "$AUDIO" ] && [ -n "$UI" ] || exit 0
ALL=0-$(( $(getconf _NPROCESSORS_CONF) - 1 ))

# The kernel's own spelling of a CPU list ("2,3" reads back as "2-3"), so a
# thread already where it belongs is recognised and not re-pinned every pass.
norm() {
    sh -c 'taskset -pc "$1" $$ >/dev/null 2>&1 &&
           sed -n "s/^Cpus_allowed_list:[[:space:]]*//p" /proc/$$/status' _ "$1"
}

is_audio() {
    case "$1" in
        JuceALSA|FGPR:*|PcmReader:*|JogCom:*|Player:*|WavWriter) return 0 ;;
    esac
    return 1
}

pin() {     # pin <tid> <cpulist-normalised>
    have=$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' "/proc/$1/status" 2>/dev/null)
    [ -n "$have" ] && [ "$have" != "$2" ] && taskset -pc "$2" "$1" >/dev/null 2>&1
    return 0
}

# Every task of every userland process except rbp's.
others() {      # others <cpus> <skip-pid>
    for p in /proc/[0-9]*; do
        p=${p#/proc/}
        [ "$p" = "$2" ] && continue
        # Kernel thread: empty cmdline (test the content -- every /proc file
        # reports size 0, so `[ -s ]` would skip every process).
        [ -n "$(head -c1 "/proc/$p/cmdline" 2>/dev/null)" ] || continue
        for t in /proc/$p/task/[0-9]*; do
            pin "${t##*/}" "$1"
        done
    done
}

# The IRQ of the host controller under the DDJ-400 (USB id 2b73:0026): its bus
# number names the controller's line in /proc/interrupts ("ehci_hcd:usb3").
ddj_irq() {
    for d in /sys/bus/usb/devices/*; do
        [ "$(cat "$d/idVendor" 2>/dev/null):$(cat "$d/idProduct" 2>/dev/null)" = 2b73:0026 ] || continue
        bus=$(cat "$d/busnum")
        awk -v u="usb$bus" '{ for (i = 1; i <= NF; i++) if ($i ~ ":"u"$") { sub(":", "", $1); print $1; exit } }' /proc/interrupts
        return
    done
}

irqs() {        # irqs <default-cpus> [<irq> <cpus>]
    for f in /proc/irq/[0-9]*/smp_affinity_list; do
        echo "$1" > "$f" 2>/dev/null     # per-CPU and chained IRQs refuse: fine
    done
    [ -n "${2:-}" ] && echo "$3" > "/proc/irq/$2/smp_affinity_list" 2>/dev/null
    return 0
}

case "${1:-}" in
    apply)
        RBP=${2:?usage: cpu-split.sh apply <rbp-pid>}
        A=$(norm "$AUDIO"); U=$(norm "$UI")
        for t in /proc/"$RBP"/task/[0-9]*; do
            c=$(cat "$t/comm" 2>/dev/null) || continue
            if is_audio "$c"; then pin "${t##*/}" "$A"; else pin "${t##*/}" "$U"; fi
            if [ "$c" = gui_task ] && [ -n "$GUI_RT" ] &&
               ! chrt -p "${t##*/}" 2>/dev/null | grep -q SCHED_FIFO; then
                chrt -f -p "$GUI_RT" "${t##*/}" 2>/dev/null &&
                    echo "cpu split: gui_task ${t##*/} SCHED_FIFO $GUI_RT"
            fi
        done
        # The rest of the system and the IRQs only need doing once per start,
        # and again only if something new turned up (cheap check: PID 1).
        if [ "$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' /proc/1/status)" != "$U" ] ||
           [ "${3:-}" = full ]; then
            others "$U" "$RBP"
            DI=$(ddj_irq)
            irqs "$U" "$DI" "$A"
            echo "cpu split: audio $A, ui+system $U${DI:+, DDJ-400 USB irq $DI -> $A}"
        fi
        ;;
    restore)
        others "$ALL" ""
        irqs "$ALL"
        echo "cpu split: everything back on $ALL"
        ;;
    *) echo "usage: $0 apply <rbp-pid> [full] | restore" >&2; exit 2 ;;
esac
