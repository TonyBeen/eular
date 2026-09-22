#!/bin/bash

set -Eeuo pipefail

SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"

INSTALL_ROOT="${NTRS_INSTALL_ROOT:-/opt/ntrs}"
STATE_ROOT="${NTRS_STATE_ROOT:-/var/lib/ntrs}"
LOG_ROOT="${NTRS_LOG_ROOT:-/var/log/ntrs}"
RELEASE_ROOT="${INSTALL_ROOT}/releases"
CURRENT_ROOT="${INSTALL_ROOT}/current"

readonly SERVICES=(ntrs natd_hub natd_node)

usage() {
    cat <<'EOF'
Usage:
  ntrs_deploy.sh install SERVICE [--url URL] [--version VERSION]
  ntrs_deploy.sh install all [--version VERSION]
  ntrs_deploy.sh start SERVICE [SERVICE_ARGS...]
  ntrs_deploy.sh stop SERVICE [-6]|all
  ntrs_deploy.sh restart SERVICE [SERVICE_ARGS...]
  ntrs_deploy.sh status SERVICE [-6]|all
  ntrs_deploy.sh clean

Environment:
  NTRS_INSTALL_ROOT  Install root (default: /opt/ntrs)
  NTRS_STATE_ROOT    PID/state directory (default: /var/lib/ntrs)
  NTRS_LOG_ROOT      Log directory (default: /var/log/ntrs)

When SERVICE is not installed, start downloads its binary from the built-in URL
before launching it. Service stdout and stderr are appended to SERVICE.log.
When SERVICE_ARGS contains -h or --help, the service runs in the foreground.

Examples:
  sudo ./ntrs_deploy.sh install ntrs
  sudo ./ntrs_deploy.sh install all
  sudo ./ntrs_deploy.sh start ntrs -a 0.0.0.0 -p 6600 -w 4
  sudo ./ntrs_deploy.sh start ntrs -6 -p 6600 -w 4
  sudo ./ntrs_deploy.sh start natd_hub --listen 0.0.0.0:7700 -i eth0
  sudo ./ntrs_deploy.sh start natd_node --hub hub.example.com:7700 --node-id node-xxx -i eth0
  sudo ./ntrs_deploy.sh start natd_node -6 --hub hub.example.com:7700 --node-id node-xxx -i eth0
EOF
}

die() {
    printf 'ntrs_deploy: %s\n' "$*" >&2
    exit 1
}

is_service() {
    case "$1" in
        ntrs|natd_hub|natd_node) return 0 ;;
        *) return 1 ;;
    esac
}

service_instance() {
    local service="$1"
    local service_arg

    shift
    for service_arg in "$@"; do
        if [[ "$service_arg" == '-6' ]]; then
            printf '%s-v6\n' "$service"
            return 0
        fi
    done
    printf '%s\n' "$service"
}

service_pid_file() {
    printf '%s/%s.pid\n' "$STATE_ROOT" "$1"
}

service_log_file() {
    printf '%s/%s.log\n' "$LOG_ROOT" "$1"
}

service_binary() {
    case "$1" in
        ntrs|natd_hub|natd_node) printf '%s/%s\n' "$CURRENT_ROOT" "$1" ;;
        *) return 1 ;;
    esac
}

service_url() {
    case "$1" in
        ntrs) printf '%s\n' 'https://www.heular.cn:1443/filebrowser/api/public/dl/7SNy23Ou/ntrs/ntrs' ;;
        natd_node) printf '%s\n' 'https://www.heular.cn:1443/filebrowser/api/public/dl/_jxpBwuP/ntrs/natd_node' ;;
        natd_hub) printf '%s\n' 'https://www.heular.cn:1443/filebrowser/api/public/dl/AuGj1Pyz/ntrs/natd_hub' ;;
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

install_one() {
    local service="$1"
    local url="$2"
    local version="$3"
    local temp_dir
    local release_dir
    local binary_path

    is_service "$service" || die "unknown service: $service"
    mkdir -p "$RELEASE_ROOT/$service" "$STATE_ROOT" "$LOG_ROOT" "$CURRENT_ROOT"
    temp_dir="$(mktemp -d "${STATE_ROOT}/download.XXXXXX")"
    release_dir="${RELEASE_ROOT}/${service}/${version}"
    binary_path="${temp_dir}/${service}"
    [[ ! -e "$release_dir" ]] || die "release already exists: $release_dir"

    printf 'Downloading %s from %s\n' "$service" "$url"
    download_file "$url" "$binary_path"
    [[ -s "$binary_path" ]] || die "downloaded binary is empty: $service"
    mkdir -p "$release_dir"
    install -m 0755 "$binary_path" "${release_dir}/${service}"
    chmod 0755 "${release_dir}/${service}"
    ln -sfn "${release_dir}/${service}" "${CURRENT_ROOT}/${service}"
    rm -rf "$temp_dir"
    printf 'Installed %s release %s at %s\n' "$service" "$version" "$release_dir"
}

install_release() {
    local service="${1:-}"
    local url=''
    local version="$(date -u +%Y%m%d%H%M%S)"
    local option

    [[ -n "$service" ]] || die 'install requires SERVICE or all'
    shift

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
    [[ "$version" =~ ^[A-Za-z0-9._-]+$ ]] || die 'version contains unsupported characters'
    if [[ "$service" == all ]]; then
        [[ -z "$url" ]] || die '--url can only be used when installing one service'
        for service in "${SERVICES[@]}"; do
            install_one "$service" "$(service_url "$service")" "$version"
        done
        return 0
    fi
    is_service "$service" || die "unknown service: $service"
    [[ -n "$url" ]] || url="$(service_url "$service")"
    install_one "$service" "$url" "$version"
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
    local instance
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
    if (($# > 0)); then
        service_args=("$@")
    fi

    binary="$(service_binary "$service")"
    [[ -x "$binary" ]] || die "missing executable: $binary; run install first"
    if ((${#service_args[@]} > 0)); then
        instance="$(service_instance "$service" "${service_args[@]}")"
    else
        instance="$(service_instance "$service")"
    fi
    pid_file="$(service_pid_file "$instance")"
    log_file="$(service_log_file "$instance")"
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
    if ((${#service_args[@]} > 0)); then
        printf ' %q' "${service_args[@]}"
    fi
    printf '\n'
    while :; do
        if ((${#service_args[@]} > 0)); then
            "$binary" "${service_args[@]}" &
        else
            "$binary" &
        fi
        child_pid="$!"
        set +e
        wait "$child_pid"
        exit_code="$?"
        set -e
        child_pid=''
        printf '%s child exited: service=%s status=%s\n' "$(date -u +%FT%TZ)" "$instance" "$exit_code"
        sleep "$restart_delay"
        if ((restart_delay < max_restart_delay)); then
            restart_delay=$((restart_delay * 2))
            ((restart_delay > max_restart_delay)) && restart_delay="$max_restart_delay"
        fi
    done
}

start_service() {
    local service="$1"
    local instance
    local pid
    local pid_file

    shift
    is_service "$service" || die "unknown service: $service"
    instance="$(service_instance "$service" "$@")"
    if is_running "$instance"; then
        die "$instance is already running"
    fi
    if [[ ! -x "$(service_binary "$service")" ]]; then
        install_release "$service"
    fi
    pid_file="$(service_pid_file "$instance")"
    rm -f "$pid_file"
    nohup /bin/bash "$SCRIPT_PATH" daemon "$service" "$@" >/dev/null 2>&1 &
    for _ in {1..20}; do
        pid="$(read_pid "$instance" 2>/dev/null || true)"
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            printf '%s started, supervisor pid=%s, log=%s\n' "$instance" "$pid" "$(service_log_file "$instance")"
            return 0
        fi
        sleep 0.1
    done
    die "failed to start $instance; inspect $(service_log_file "$instance")"
}

run_service_foreground() {
    local service="$1"
    local binary

    shift
    is_service "$service" || die "unknown service: $service"
    binary="$(service_binary "$service")"
    if [[ ! -x "$binary" ]]; then
        install_release "$service"
    fi
    "$binary" "$@"
}

stop_service() {
    local service="$1"
    local instance
    local pid

    shift
    is_service "$service" || die "unknown service: $service"
    instance="$(service_instance "$service" "$@")"
    if ! pid="$(read_pid "$instance" 2>/dev/null)"; then
        printf '%s is not running\n' "$instance"
        return 0
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
        rm -f "$(service_pid_file "$instance")"
        printf '%s is not running\n' "$instance"
        return 0
    fi
    kill "$pid"
    for _ in {1..50}; do
        if ! kill -0 "$pid" 2>/dev/null; then
            printf '%s stopped\n' "$instance"
            return 0
        fi
        sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null || true
    rm -f "$(service_pid_file "$instance")"
    printf '%s stopped forcefully\n' "$instance"
}

status_service() {
    local service="$1"
    local instance
    local pid

    shift
    is_service "$service" || die "unknown service: $service"
    instance="$(service_instance "$service" "$@")"
    if pid="$(read_pid "$instance" 2>/dev/null)" && kill -0 "$pid" 2>/dev/null; then
        printf '%s running supervisor_pid=%s log=%s\n' "$instance" "$pid" "$(service_log_file "$instance")"
    else
        printf '%s stopped\n' "$instance"
    fi
}

clean_installation() {
    local service

    for service in "${SERVICES[@]}"; do
        stop_service "$service"
    done
    for service in "${SERVICES[@]}"; do
        stop_service "$service" -6
    done

    rm -rf -- "$RELEASE_ROOT" "$CURRENT_ROOT" "$STATE_ROOT" "$LOG_ROOT"
    rm -f -- "$SCRIPT_PATH"
    printf 'ntrs_deploy: removed installed services, logs, state, and %s\n' "$SCRIPT_PATH"
}

main() {
    local command="${1:-}"
    local service
    local help_requested
    local service_arg

    [[ -n "$command" ]] || { usage; exit 2; }
    shift
    case "$command" in
        install) install_release "$@" ;;
        start)
            (($# > 0)) || die 'start requires a service'
            service="$1"
            shift
            help_requested=false
            for service_arg in "$@"; do
                case "$service_arg" in
                    -h|--help) help_requested=true; break ;;
                esac
            done
            if [[ "$help_requested" == true ]]; then
                run_service_foreground "$service" "$@"
            else
                start_service "$service" "$@"
            fi
            ;;
        stop|status)
            (($# >= 1 && $# <= 2)) || die "$command requires SERVICE, optional -6, or all"
            service="$1"
            shift
            if [[ "$service" == all ]]; then
                (($# == 0)) || die "$command all does not accept -6"
                for service in "${SERVICES[@]}"; do
                    if [[ "$command" == stop ]]; then
                        stop_service "$service"
                    else
                        status_service "$service"
                    fi
                done
                for service in natd_hub natd_node; do
                    if [[ "$command" == stop ]]; then
                        stop_service "$service" -6
                    else
                        status_service "$service" -6
                    fi
                done
            elif [[ "$command" == stop ]]; then
                if (($# == 1)) && [[ "$1" != '-6' ]]; then
                    die "$command accepts only -6"
                fi
                stop_service "$service" "$@"
            else
                if (($# == 1)) && [[ "$1" != '-6' ]]; then
                    die "$command accepts only -6"
                fi
                status_service "$service" "$@"
            fi
            ;;
        restart)
            (($# > 0)) || die 'restart requires a service'
            service="$1"
            shift
            stop_service "$service" "$@"
            start_service "$service" "$@"
            ;;
        clean)
            (($# == 0)) || die 'clean does not accept arguments'
            clean_installation
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
