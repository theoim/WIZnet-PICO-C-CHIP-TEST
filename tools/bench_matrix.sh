#!/usr/bin/env bash
#
# Build the crypto benchmark for one part at a time, with and without the
# mbedTLS assembly bignum path, and stage the images under names that say what
# they are.
#
# Why a script rather than four cmake invocations by hand: this repository
# already contains five build directories that each produce a crypto_bench.uf2,
# none of which records which part or which mbedTLS configuration it came from.
# A measurement attributed to the wrong image is worse than no measurement, so
# every variant here gets its own build directory, its own staged file name, and
# a manifest line carrying the md5 of the exact bytes that were flashed. The
# image prints the same facts over USB when it starts.
#
# Usage:
#   tools/bench_matrix.sh                 # build all four variants
#   tools/bench_matrix.sh rp2040          # only the RP2040 pair
#   tools/bench_matrix.sh rp2350_asm      # one variant
#   tools/bench_matrix.sh --list          # show the matrix and exit
#   tools/bench_matrix.sh --flash rp2350_asm
#                                         # copy that image to a board waiting
#                                         # in BOOTSEL (asks first)
#   tools/bench_matrix.sh --flash rp2040_noasm COM17
#                                         # same, but reboot a running board
#                                         # into BOOTSEL first, over its serial
#                                         # port - no cable handling
#
# The boards: rp2040 builds for W55RP20_EVB_PICO, rp2350 for W6300_EVB_PICO2.
# The benchmark links only pico_stdlib and mbedTLS, so the Ethernet part is
# irrelevant to the numbers - the board name only picks the MCU and the pin
# definitions. Both are set below if a different pair is wanted.

set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$REPO/build_bench_out"

BOARD_RP2040="W55RP20_EVB_PICO"
BOARD_RP2350="W6300_EVB_PICO2"

# variant | board | BENCH_NO_ASM
#
# A variant whose name ends in _ram is built to run from RAM instead of
# executing in place from flash (see the crypto_bench CMakeLists for why).
# Deriving it from the name keeps this table to three columns.
VARIANTS="
rp2040_asm   : $BOARD_RP2040 : 0
rp2040_noasm : $BOARD_RP2040 : 1
rp2040_ram   : $BOARD_RP2040 : 0
rp2350_asm   : $BOARD_RP2350 : 0
rp2350_noasm : $BOARD_RP2350 : 1
rp2350_ram   : $BOARD_RP2350 : 0
"

die() { printf 'bench_matrix: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# nmake is the generator this project is configured with, and it is not usable
# from PATH on this machine - PATH carries the executable itself as an entry,
# which the shell cannot use as a directory. Locate it and hand cmake the
# absolute path instead of asking for a Developer Prompt.
# ---------------------------------------------------------------------------
find_nmake() {
    # cmake is a native Windows binary and cannot follow an MSYS /c/... path.
    # cygpath -m returns C:/Program Files/... - a Windows path with forward
    # slashes, which both the shell and cmake accept. It still contains spaces,
    # so every use of the result has to stay quoted.
    if [ -n "${NMAKE:-}" ] && [ -x "$NMAKE" ]; then cygpath -m "$NMAKE"; return 0; fi
    local c
    for c in "/c/Program Files/Microsoft Visual Studio"/*/*/VC/Tools/MSVC/*/bin/Hostx86/x86/nmake.exe \
             "/c/Program Files (x86)/Microsoft Visual Studio"/*/*/VC/Tools/MSVC/*/bin/Hostx86/x86/nmake.exe; do
        if [ -x "$c" ]; then cygpath -m "$c"; return 0; fi
    done
    return 1
}

bignum_label() {
    if [ "$1" = "1" ]; then printf 'C'; else printf 'assembly'; fi
}

# A trailing _ram in the variant name means "run from RAM".
wants_ram() {
    case "$1" in
        *_ram) return 0 ;;
        *)     return 1 ;;
    esac
}

code_label() {
    if wants_ram "$1"; then printf 'RAM'; else printf 'flash'; fi
}

list_matrix() {
    printf '%-14s %-20s %s\n' VARIANT BOARD BIGNUM
    while IFS=: read -r v b a; do
        v="$(echo "$v" | xargs)"; b="$(echo "$b" | xargs)"; a="$(echo "$a" | xargs)"
        if [ -z "$v" ]; then continue; fi
        printf '%-14s %-20s %s\n' "$v" "$b" "$(bignum_label "$a")"
    done <<< "$VARIANTS"
}

# ---------------------------------------------------------------------------
# The pre-existing build directories are left exactly as they are: they hold
# the only copies of earlier work. They are listed once so that a stale uf2 is
# never picked up by mistake.
# ---------------------------------------------------------------------------
warn_old_builds() {
    local found=0 d
    for d in "$REPO"/build "$REPO"/build_2350 "$REPO"/build_bench \
             "$REPO"/build_opcua_main "$REPO"/build_w55rp20; do
        if [ ! -d "$d" ]; then continue; fi
        if [ "$found" -eq 0 ]; then
            echo
            echo "Other build directories here (not touched, do not flash from them):"
            found=1
        fi
        printf '  %s\n' "${d#$REPO/}"
    done
    if [ "$found" -eq 1 ]; then
        echo "  -> this script writes only build_bench_<variant>/ and build_bench_out/"
    fi
}

build_one() {
    local variant="$1" board="$2" noasm="$3"
    local dir="$REPO/build_bench_$variant"

    echo
    echo "=============================================================="
    echo " $variant   board=$board   BENCH_NO_ASM=$noasm   code=$(code_label "$variant")"
    echo "=============================================================="

    local args
    args=(-S "$REPO" -B "$dir"
          -G "NMake Makefiles"
          -DCMAKE_MAKE_PROGRAM="$NMAKE_BIN"
          -DBOARD_NAME="$board"
          -DPICO_SDK_PATH="$REPO/libraries/pico-sdk")
    if [ "$noasm" = "1" ]; then args+=(-DBENCH_NO_ASM=1); fi
    if wants_ram "$variant"; then args+=(-DBENCH_RAM=1); fi

    if ! cmake "${args[@]}" > "$dir.configure.log" 2>&1; then
        tail -25 "$dir.configure.log"
        die "configure failed for $variant - full log in $dir.configure.log"
    fi

    if ! cmake --build "$dir" --target crypto_bench > "$dir.build.log" 2>&1; then
        tail -25 "$dir.build.log"
        die "build failed for $variant - full log in $dir.build.log"
    fi

    local uf2="$dir/examples/crypto_bench/crypto_bench.uf2"
    if [ ! -f "$uf2" ]; then die "no uf2 produced for $variant"; fi

    mkdir -p "$OUT"
    cp "$uf2" "$OUT/crypto_bench_$variant.uf2"

    # The code size belongs next to the timing: dropping the assembly path
    # changes size as well as speed, and the security plan wants a Flash figure
    # anyway.
    local elf="$dir/examples/crypto_bench/crypto_bench.elf"
    arm-none-eabi-size -A -d "$elf" > "$OUT/crypto_bench_$variant.size.txt" 2>/dev/null

    local md5 size
    md5="$(md5sum "$OUT/crypto_bench_$variant.uf2" | cut -d' ' -f1)"
    size="$(stat -c %s "$OUT/crypto_bench_$variant.uf2")"
    printf '%-14s %-20s %-9s %-6s %-34s %10s  %s\n' \
        "$variant" "$board" "$(bignum_label "$noasm")" "$(code_label "$variant")" \
        "$md5" "$size" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$OUT/manifest.txt"

    echo "  staged  build_bench_out/crypto_bench_$variant.uf2"
    echo "  md5     $md5"
}

# ---------------------------------------------------------------------------
# Flashing. A board in BOOTSEL mounts as a drive holding INFO_UF2.TXT, and that
# file names the part, which is checked against the variant so an RP2040 image
# is not dropped onto an RP2350 or the other way round.
# ---------------------------------------------------------------------------
find_bootsel_drive() {
    local d
    for d in /d /e /f /g /h /i /j /k /l /m /n /o /p /q /r /s /t /u /v /w /x /y /z; do
        if [ -f "$d/INFO_UF2.TXT" ]; then printf '%s' "$d"; return 0; fi
    done
    return 1
}

# Opening the USB serial port at 1200 baud reboots the board into BOOTSEL:
# pico_stdio_usb watches for that baud rate and resets
# (PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE, on by default). It saves
# unplugging the board between the two images of an A/B pair, which is worth
# having when the whole point is that nothing else changed between them.
reset_to_bootsel() {
    local port="$1"
    echo "  sending the BOOTSEL reset to $port (1200 baud)"
    powershell -NoProfile -Command \
        "try { \$p = New-Object System.IO.Ports.SerialPort '$port',1200; \$p.Open(); Start-Sleep -Milliseconds 200; \$p.Close() } catch { exit 1 }" \
        > /dev/null 2>&1

    local i drive
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
        sleep 1
        drive="$(find_bootsel_drive)" && { printf '%s' "$drive"; return 0; }
    done
    return 1
}

flash_one() {
    local variant="$1" port="${2:-}"
    local img="$OUT/crypto_bench_$variant.uf2"
    if [ ! -f "$img" ]; then die "no staged image for $variant - build it first"; fi

    local drive
    drive="$(find_bootsel_drive)" || drive=""

    if [ -z "$drive" ] && [ -n "$port" ]; then
        drive="$(reset_to_bootsel "$port")" || drive=""
    fi

    if [ -z "$drive" ]; then
        if [ -n "$port" ]; then
            die "$port did not come back as a BOOTSEL drive - hold BOOTSEL and replug instead"
        fi
        die "no board in BOOTSEL found. Either hold BOOTSEL while plugging it in, or pass the serial port it is on: --flash $variant COM17"
    fi

    local model
    model="$(tr -d '\r' < "$drive/INFO_UF2.TXT" | grep -i '^Model:' | head -1)"
    echo
    echo "  board in BOOTSEL : $drive   $model"
    echo "  image            : crypto_bench_$variant.uf2"

    case "$variant:$model" in
        rp2040_*RP2350*|rp2350_*RP2040*)
            die "part mismatch - that image is for the other part" ;;
    esac

    # Only ask when there is someone to answer. Run from a non-interactive
    # shell the read returns immediately with nothing, which used to look like
    # a refusal and silently skipped the copy - naming the variant on the
    # command line is already the decision.
    if [ -t 0 ]; then
        printf '  copy it? [y/N] '
        read -r a
        if [ "$a" != y ] && [ "$a" != Y ]; then echo "  skipped"; return 0; fi
    else
        echo "  (no terminal to ask - the variant was named, so copying)"
    fi

    cp "$img" "$drive/" && echo "  copied - the board reboots and prints its banner over USB"
}

# ---------------------------------------------------------------------------

case "${1:-}" in
    --list)
        list_matrix
        exit 0
        ;;
    --flash)
        if [ -z "${2:-}" ]; then die "--flash needs a variant name (see --list)"; fi
        flash_one "$2" "${3:-}"
        exit $?
        ;;
esac

NMAKE_BIN="$(find_nmake)" || die "nmake.exe not found - set NMAKE=/path/to/nmake.exe"
command -v cmake > /dev/null || die "cmake not on PATH"
command -v arm-none-eabi-gcc > /dev/null || die "arm-none-eabi-gcc not on PATH"

filter="${1:-}"
warn_old_builds

mkdir -p "$OUT"
{
    echo "# crypto_bench images staged by tools/bench_matrix.sh"
    printf '# repo HEAD: %s' "$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null)"
    if git -C "$REPO" status --porcelain 2>/dev/null | grep -q .; then
        printf ' (dirty working tree)'
    fi
    printf '\n'
    printf '%-14s %-20s %-9s %-6s %-34s %10s  %s\n' VARIANT BOARD BIGNUM CODE MD5 BYTES STAGED_UTC
} > "$OUT/manifest.txt"

built=0
while IFS=: read -r v b a; do
    v="$(echo "$v" | xargs)"; b="$(echo "$b" | xargs)"; a="$(echo "$a" | xargs)"
    if [ -z "$v" ]; then continue; fi
    case "$v" in
        $filter*) ;;
        *) continue ;;
    esac
    build_one "$v" "$b" "$a"
    built=$((built+1))
done <<< "$VARIANTS"

if [ "$built" -eq 0 ]; then die "no variant matched '$filter' (see --list)"; fi

echo
echo "=============================================================="
cat "$OUT/manifest.txt"
echo "=============================================================="
echo
echo "Next, one variant at a time:"
echo
echo "  1. hold BOOTSEL, plug the board in"
echo "  2. tools/bench_matrix.sh --flash <variant>"
echo "  3. open the USB serial port and wait - the banner comes out first, and"
echo "     it must name the part and the bignum setting you meant to measure"
echo "  4. paste the output back; the figure that decides the design is the"
echo "     OpenSecureChannel total at the end, not the individual operations"
