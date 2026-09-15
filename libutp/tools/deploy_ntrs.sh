#!/usr/bin/env bash

set -Eeuo pipefail

INSTALL_ROOT="${NTRS_INSTALL_ROOT:-/opt/eular/ntrs}"
STATE_ROOT="${NTRS_STATE_ROOT:-/var/lib/eular-ntrs}"
LOG_ROOT="${NTRS_LOG_ROOT:-/var/log/eular-ntrs}"
RELEASE_ROOT="${INSTALL_ROOT}/releases"
CURRENT_LINK="${INSTALL_ROOT}/current"

readonly SERVICES=(ntrs natd_hub natd_node)

usage() {
    cat <<'EOF'
Usage:
  deploy_ntrs.sh install --url URL [--version VERSION]
  deploy_ntrs.sh start SERVICE [-- SERVICE_ARGS...]
  deploy_ntrs.sh stop SERVICE|all
  deploy_ntrs.sh restart SERVICE [-- SERVICE_ARGS...]
  deploy_ntrs.sh status SERVICE|all

Environment:
  NTRS_INSTALL_ROOT  Install root (default: /opt/eular/ntrs)
  NTRS_STATE_ROOT    PID/state directory (default: /var/lib/eular-ntrs)
  NTRS_LOG_ROOT      Log directory (default: /var/log/eular-ntrs)

Examples:
  sudo ./tools/deploy_ntrs.sh install --url https://host/releases/ntrs-linux.tar.gz
  sudo ./tools/deploy_ntrs.sh start ntrs -- -a 0.0.0.0 -p 6600 -w 4
  sudo ./tools/deploy_ntrs.sh start natd_hub -- --listen 0.0.0.0:7700 -i eth0
  sudo ./tools/deploy_ntrs.sh start natd_node -- --hub hub.example.com:7700 --node-id node-1 -i eth0
EOF
}

die() {
    printf 'deploy_ntrs: %s\n' "$*" >&2
    exit 1
}

is_service() {
    case "$1" in
        ntrs|natd_hub|natd_node) return 0 ;;
        *) return 1 ;;
    esac
}

service_pid_file() {
    printf '%s/%s.pid\n' "$STATE_ROOT" "$1"
}

service_log_file() {
    printf '%s/%s.log\n' "$LOG_ROOT" "$1"
}

service_binary() {
    case "$1" in
        ntrs|natd_hub|natd_node) printf '%s/%s\n' "$CURRENT_LINK" "$1" ;;
        *) return 1 ;;
    esac
}

download_file() {
    local url="$1"
    local output="$2"

    if command -v curl >/dev/null 2>&1; then
        curl --fail --location --retry 3 --connect-timeout 15 --output "$output" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget --tries=3 --timeout=15 --output-document="$output" "$url"
    else
        die 'curl or wget is required for installation'
    fi
}

install_release() {
    local url=''
    local version="$(date -u +%Y%m%d%H%M%S)"
    local option
    local value
    local temp_dir
    local archive
    local release_dir
    local binary
    local source

    while (($# > 0)); do
        option="$1"
        shift
        case "$option" in
            --url)
                (($# > 0)) || die '--url requires a value'
                url="$1"
                shift
                ;;
            --version)
                (($# > 0)) || die '--version requires a value'
                version="$1"
                shift
                ;;
            *) die "unknown install option: $option" ;;
        esac
    done
    [[ -n "$url" ]] || die 'install requires --url URL'
    [[ "$version" =~ ^[A-Za-z0-9._-]+$ ]] || die 'version contains unsupported characters'

    mkdir -p "$RELEASE_ROOT" "$STATE_ROOT" "$LOG_ROOT"
    temp_dir="$(mktemp -d "${STATE_ROOT}/download.XXXXXX")"
    trap 'rm -rf "${temp_dir:-}"' EXIT
    archive="${temp_dir}/release.tar"
    release_dir="${RELEASE_ROOT}/${version}"

    printf 'Downloading %s\n' "$url"
    download_file "$url" "$archive"
    mkdir -p "${temp_dir}/extract"
    tar -xf "$archive" -C "${temp_dir}/extract"
    [[ ! -e "$release_dir" ]] || die "release already exists: $release_dir"
    mkdir -p "$release_dir"

    for binary in "${SERVICES[@]}"; do
        source="$(find "${temp_dir}/extract" -type f -name "$binary" -print -quit)"
        [[ -n "$source" ]] || die "release does not contain required binary: $binary"
        install -m 0755 "$source" "${release_dir}/${binary}"
    done
    for binary in nat_punch ntrs_natc; do
        source="$(find "${temp_dir}/extract" -type f -name "$binary" -print -quit)"
        if [[ -n "$source" ]]; then
            install -m 0755 "$source" "${release_dir}/${binary}"
        fi
    done
    chmod 0755 "${release_dir}"/*
    ln -sfn "$release_dir" "$CURRENT_LINK"
    rm -rf "$temp_dir"
    trap - EXIT
    printf 'Installed release %s at %s\n' "$version" "$release_dir"
}

read_pid() {
    local service="$1"
    local pid_file

    pid_file="$(service_pid_file "$service")"
    [[ -s "$pid_file" ]] || return 1
    read -r pid < "$pid_file"
    [[ "$pid" =~ ^[0-9]+$ ]] || return 1
    printf '%s\n' "$pid"
}

is_running() {
    local pid

    pid="$(read_pid "$1" 2>/dev/null || true)"
    [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null
}

daemon() {
    local service="$1"
    local binary
    local pid_file
    local log_file
    local child_pid=''
    local exit_code
    local restart_delay=1
    local max_restart_delay=30
    local -a service_args=()

    shift
    is_service "$service" || die "unknown service: $service"
    while (($# > 0)); do
        if [[ "$1" == '--' ]]; then
            shift
            break
        fi
        service_args+=("$1")
        shift
    done
    service_args+=("$@")

    binary="$(service_binary "$service")"
    [[ -x "$binary" ]] || die "missing executable: $binary; run install first"
    pid_file="$(service_pid_file "$service")"
    log_file="$(service_log_file "$service")"
    mkdir -p "$STATE_ROOT" "$LOG_ROOT"
    printf '%s\n' "$$" > "$pid_file"
    exec >> "$log_file" 2>&1

    stop_child() {
        if [[ -n "$child_pid" ]] && kill -0 "$child_pid" 2>/dev/null; then
            kill "$child_pid" 2>/dev/null || true
            wait "$child_pid" 2>/dev/null || true
        fi
    }
    cleanup() {
        stop_child
        rm -f "$pid_file"
    }
    trap cleanup EXIT
    trap 'exit 143' TERM INT

    printf '%s supervisor started: %s' "$(date -u +%FT%TZ)" "$binary"
    printf ' %q' "${service_args[@]}"
    printf '\n'
    while :; do
        "$binary" "${service_args[@]}" &
        child_pid="$!"
        set +e
        wait "$child_pid"
        exit_code="$?"
        set -e
        child_pid=''
        printf '%s child exited: service=%s status=%s\n' "$(date -u +%FT%TZ)" "$service" "$exit_code"
        sleep "$restart_delay"
        if ((restart_delay < max_restart_delay)); then
            restart_delay=$((restart_delay * 2))
            ((restart_delay > max_restart_delay)) && restart_delay="$max_restart_delay"
        fi
    done
}

start_service() {
    local service="$1"
    local pid
    local pid_file

    shift
    is_service "$service" || die "unknown service: $service"
    if is_running "$service"; then
        die "$service is already running"
    fi
    pid_file="$(service_pid_file "$service")"
    rm -f "$pid_file"
    nohup "$0" daemon "$service" -- "$@" >/dev/null 2>&1 &
    for _ in {1..20}; do
        if pid="$(read_pid "$service" 2>/dev/null || true)"; then
            printf '%s started, supervisor pid=%s, log=%s\n' "$service" "$pid" "$(service_log_file "$service")"
            return 0
        fi
        sleep 0.1
    done
    die "failed to start $service; inspect $(service_log_file "$service")"
}

stop_service() {
    local service="$1"
    local pid

    is_service "$service" || die "unknown service: $service"
    if ! pid="$(read_pid "$service" 2>/dev/null)"; then
        printf '%s is not running\n' "$service"
        return 0
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
        rm -f "$(service_pid_file "$service")"
        printf '%s is not running\n' "$service"
        return 0
    fi
    kill "$pid"
    for _ in {1..50}; do
        if ! kill -0 "$pid" 2>/dev/null; then
            printf '%s stopped\n' "$service"
            return 0
        fi
        sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null || true
    rm -f "$(service_pid_file "$service")"
    printf '%s stopped forcefully\n' "$service"
}

status_service() {
    local service="$1"
    local pid

    is_service "$service" || die "unknown service: $service"
    if pid="$(read_pid "$service" 2>/dev/null)" && kill -0 "$pid" 2>/dev/null; then
        printf '%s running supervisor_pid=%s log=%s\n' "$service" "$pid" "$(service_log_file "$service")"
    else
        printf '%s stopped\n' "$service"
    fi
}

main() {
    local command="${1:-}"
    local service

    [[ -n "$command" ]] || { usage; exit 2; }
    shift
    case "$command" in
        install) install_release "$@" ;;
        start)
            (($# > 0)) || die 'start requires a service'
            service="$1"
            shift
            [[ "${1:-}" != '--' ]] || shift
            start_service "$service" "$@"
            ;;
        stop|status)
            (($# == 1)) || die "$command requires SERVICE or all"
            service="$1"
            if [[ "$service" == all ]]; then
                for service in "${SERVICES[@]}"; do
                    if [[ "$command" == stop ]]; then stop_service "$service"; else status_service "$service"; fi
                done
            elif [[ "$command" == stop ]]; then
                stop_service "$service"
            else
                status_service "$service"
            fi
            ;;
        restart)
            (($# > 0)) || die 'restart requires a service'
            service="$1"
            shift
            [[ "${1:-}" != '--' ]] || shift
            stop_service "$service"
            start_service "$service" "$@"
            ;;
        daemon)
            (($# > 0)) || die 'daemon requires a service'
            daemon "$@"
            ;;
        help|-h|--help) usage ;;
        *) usage >&2; exit 2 ;;
    esac
}

main "$@"
