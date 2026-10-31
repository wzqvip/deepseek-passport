#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

# The interpreter this gate calls is `python` on Windows and `python3` almost everywhere else, and Git
# Bash on Windows has no `python3` alias at all. Selecting it once keeps the gate runnable on the
# developer's machine instead of only in CI, which is what makes "CI runs the same script" true.
if [[ -n "${PYTHON:-}" ]]; then
    :
elif command -v python3 >/dev/null 2>&1; then
    PYTHON=python3
elif command -v python >/dev/null 2>&1; then
    PYTHON=python
else
    echo "ERROR: no python3 or python on PATH" >&2
    exit 1
fi

# ESP-IDF's own tools print an unsupported-environment warning under MSys/MingW and then produce nothing,
# so a firmware run started from Git Bash on Windows would quietly verify a directory that was never built.
# Refusing up front, with the working alternative named, is better than a failure that looks like a broken
# build. The static mode is unaffected: it needs a C compiler and Python only.
refuse_msys_firmware() {
    if [[ -z "${MSYSTEM:-}" ]]; then
        return 0
    fi
    echo "ERROR: the firmware mode cannot run under MSys/MingW (Git Bash)." >&2
    echo "       Static checks:  bash tools/validate.sh --static" >&2
    echo "       Firmware, from an ESP-IDF 5.5.3 PowerShell or export.sh prompt:" >&2
    echo "         idf.py -B <dir> -D SDKCONFIG=<dir>/sdkconfig build" >&2
    echo "         idf.py -B <dir> merge-bin -o FoloToy-AI-Passport-full.bin" >&2
    echo "         python tools/verify_firmware.py <dir>" >&2
    echo "         python tools/archive_firmware.py create <dir> --archive-root build/firmware" >&2
    exit 1
}

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin
    local test_dir

    "${PYTHON}" tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d /tmp/ai-passport-host-tests.XXXXXX)"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_passport_metrics_math.c main/passport_metrics_math.c \
        -o "${test_dir}/test_passport_metrics_math"
    "${test_dir}/test_passport_metrics_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_passport_audio_arbiter.c main/passport_audio_arbiter.c \
        -o "${test_dir}/test_passport_audio_arbiter"
    "${test_dir}/test_passport_audio_arbiter"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_passport_config.c main/passport_config.c \
        -o "${test_dir}/test_passport_config"
    "${test_dir}/test_passport_config"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_passport_json.c main/passport_json.c \
        -o "${test_dir}/test_passport_json"
    "${test_dir}/test_passport_json"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_passport_utf8.c main/passport_utf8.c \
        -o "${test_dir}/test_passport_utf8"
    "${test_dir}/test_passport_utf8"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_passport_portal.c main/passport_portal.c main/passport_config.c \
        -o "${test_dir}/test_passport_portal"
    "${test_dir}/test_passport_portal"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_passport_qr.c main/passport_qr.c \
        -o "${test_dir}/test_passport_qr"
    "${test_dir}/test_passport_qr"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_passport_voice.c main/passport_voice.c \
        -o "${test_dir}/test_passport_voice"
    "${test_dir}/test_passport_voice"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_passport_work.c main/passport_work.c main/passport_json.c \
        -o "${test_dir}/test_passport_work"
    "${test_dir}/test_passport_work"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_passport_navigation.c main/passport_navigation.c \
        -o "${test_dir}/test_passport_navigation"
    "${test_dir}/test_passport_navigation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_passport_gate.c main/passport_gate.c \
        -o "${test_dir}/test_passport_gate"
    "${test_dir}/test_passport_gate"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_passport_provider.c main/passport_provider.c \
        -o "${test_dir}/test_passport_provider"
    "${test_dir}/test_passport_provider"
    # -lm: the test checks the integer approximation against the library's sine; the firmware
    # itself uses no floating point.
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_passport_tone.c main/passport_tone.c \
        -o "${test_dir}/test_passport_tone" -lm
    "${test_dir}/test_passport_tone"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_passport_identity.c main/passport_identity.c \
        -o "${test_dir}/test_passport_identity"
    "${test_dir}/test_passport_identity"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_key_guard.c -o "${test_dir}/test_key_guard"
    "${test_dir}/test_key_guard"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_es8311_sleep_check"
    "${test_dir}/test_bsp_es8311_sleep_check"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${test_dir}/test_bsp_button"
    "${test_dir}/test_bsp_button"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_lvgl_init"
    "${test_dir}/test_bsp_lvgl_init"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_audio_recovery"
    "${test_dir}/test_bsp_audio_recovery"
    # The demo runtime tests were removed with the demo interface they exercised: they included
    # main/demo_audio.c, demo_ble.c, demo_low_power.c and demo_wifi.c directly, and those pages were
    # replaced by the application's own screens. Their deep-sleep contract was not deleted with them —
    # it was retargeted at main/passport_power.c, because it is what found that the replacement was
    # sleeping without shutting the board down first.
    PYTHONDONTWRITEBYTECODE=1 "${PYTHON}" tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 "${PYTHON}" tests/test_key_wake_contract.py
    PYTHONDONTWRITEBYTECODE=1 "${PYTHON}" tests/test_passport_chat_contract.py
    PYTHONDONTWRITEBYTECODE=1 "${PYTHON}" tests/test_passport_bridge.py
    PYTHONDONTWRITEBYTECODE=1 "${PYTHON}" tests/test_passport_work_companion.py
    PYTHONDONTWRITEBYTECODE=1 "${PYTHON}" tests/test_start_bridge.py
    PYTHONDONTWRITEBYTECODE=1 "${PYTHON}" tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 "${PYTHON}" tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 "${PYTHON}" tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 "${PYTHON}" tests/test_install_passport_skills.py
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_build_dir

    refuse_msys_firmware

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    "${PYTHON}" tools/verify_firmware.py "${validation_build_dir}"
    PYTHONDONTWRITEBYTECODE=1 "${PYTHON}" tools/archive_firmware.py create \
        "${validation_build_dir}" --archive-root "${repo_root}/build/firmware"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
