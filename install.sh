#!/usr/bin/env bash
# install.sh — 프로젝트 의존성 설치 및 빌드 스크립트
# 사용법: bash install.sh [--build] [--test]
#   --build : 설치 후 broker 빌드 + client npm install 까지 수행
#   --test  : --build 포함, 이후 ctest 까지 수행
set -euo pipefail

# ── 색상 헬퍼 ─────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; RESET='\033[0m'
info()    { printf "${CYAN}[info]${RESET}  %s\n" "$*"; }
ok()      { printf "${GREEN}[ok]${RESET}    %s\n" "$*"; }
warn()    { printf "${YELLOW}[warn]${RESET}  %s\n" "$*"; }
error()   { printf "${RED}[error]${RESET} %s\n" "$*" >&2; }
die()     { error "$*"; exit 1; }

# ── 옵션 파싱 ─────────────────────────────────────────────────────────────────
OPT_BUILD=false
OPT_TEST=false
for arg in "$@"; do
  case "$arg" in
    --build) OPT_BUILD=true ;;
    --test)  OPT_BUILD=true; OPT_TEST=true ;;
    -h|--help)
      echo "사용법: bash install.sh [--build] [--test]"
      echo "  (옵션 없음) 의존성 설치만 수행"
      echo "  --build    의존성 설치 + broker 빌드 + client npm install"
      echo "  --test     --build 포함 + ctest 실행"
      exit 0 ;;
    *) die "알 수 없는 옵션: $arg (--help 참조)" ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# ── 플랫폼 감지 ───────────────────────────────────────────────────────────────
OS="$(uname -s)"
ARCH="$(uname -m)"
info "플랫폼: $OS $ARCH"

if [[ "$OS" == "Darwin" ]]; then
  PLATFORM="macos"
elif [[ "$OS" == "Linux" ]]; then
  if command -v apt-get &>/dev/null; then
    PLATFORM="debian"
  elif command -v dnf &>/dev/null; then
    PLATFORM="fedora"
  elif command -v yum &>/dev/null; then
    PLATFORM="rhel"
  else
    die "지원하지 않는 Linux 배포판 (apt/dnf/yum 필요)"
  fi
else
  die "지원하지 않는 OS: $OS"
fi

# ── Y/N 확인 헬퍼 ─────────────────────────────────────────────────────────────
ask_install() {
  local pkg_desc="$1"
  local response
  printf "${YELLOW}[?]${RESET}    %s 이(가) 설치되어 있지 않습니다. 설치하시겠습니까? [Y/n] " "$pkg_desc"
  read -r response
  case "${response:-Y}" in
    [Yy]*) return 0 ;;
    *)     return 1 ;;
  esac
}

# ── 패키지 설치 함수 ──────────────────────────────────────────────────────────
declare -A INSTALLED_PKGS=()

install_pkg_macos() {
  local brew_pkg="$1"
  info "brew install $brew_pkg ..."
  brew install "$brew_pkg"
}

install_pkg_debian() {
  local apt_pkg="$1"
  info "apt-get install $apt_pkg ..."
  sudo apt-get update -qq
  sudo apt-get install -y "$apt_pkg"
}

install_pkg_fedora() {
  local dnf_pkg="$1"
  info "dnf install $dnf_pkg ..."
  sudo dnf install -y "$dnf_pkg"
}

install_pkg_rhel() {
  local yum_pkg="$1"
  info "yum install $yum_pkg ..."
  sudo yum install -y "$yum_pkg"
}

install_pkg() {
  local desc="$1" macos_pkg="$2" linux_pkg="$3"
  local pkg
  case "$PLATFORM" in
    macos) pkg="$macos_pkg" ;;
    *)     pkg="$linux_pkg" ;;
  esac

  if [[ -n "${INSTALLED_PKGS[$pkg]+x}" ]]; then
    ok "$desc (패키지 '$pkg' 이미 설치됨)"
    return
  fi

  case "$PLATFORM" in
    macos)  install_pkg_macos   "$macos_pkg" ;;
    debian) install_pkg_debian  "$linux_pkg" ;;
    fedora) install_pkg_fedora  "$linux_pkg" ;;
    rhel)   install_pkg_rhel    "$linux_pkg" ;;
  esac
  INSTALLED_PKGS[$pkg]=1
  ok "$desc 설치 완료"
}

# ── Homebrew (macOS 전용) ──────────────────────────────────────────────────────
if [[ "$PLATFORM" == "macos" ]]; then
  if ! command -v brew &>/dev/null; then
    if ask_install "Homebrew"; then
      info "Homebrew 설치 중 ..."
      /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
      ok "Homebrew 설치 완료"
    else
      die "Homebrew 없이는 macOS에서 의존성을 설치할 수 없습니다."
    fi
  else
    ok "Homebrew $(brew --version | head -1)"
  fi
fi

# ── 의존성 목록 정의 ──────────────────────────────────────────────────────────
# [cmd_to_check, description, macos_pkg, linux_pkg]
declare -a DEPS_CMD=()
declare -a DEPS_DESC=()
declare -a DEPS_MACOS=()
declare -a DEPS_LINUX=()

add_dep() {
  DEPS_CMD+=("$1"); DEPS_DESC+=("$2"); DEPS_MACOS+=("$3"); DEPS_LINUX+=("$4")
}

# 빌드 도구
add_dep "cmake"       "CMake (브로커 빌드)"         "cmake"          "cmake"
add_dep "make"        "Make"                         "make"           "build-essential"
add_dep "pkg-config"  "pkg-config (라이브러리 탐색)" "pkg-config"     "pkg-config"

# C++ 컴파일러
if [[ "$PLATFORM" == "macos" ]]; then
  add_dep "clang++"   "Clang++ (C++17 컴파일러)"    "llvm"           "clang"
else
  add_dep "g++"       "GCC/G++ (C++17 컴파일러)"    "gcc"            "g++"
fi

# MQTT 브로커 + 라이브러리
add_dep "mosquitto"       "Mosquitto MQTT 브로커"       "mosquitto"      "mosquitto"
add_dep "mosquitto_pub"   "Mosquitto 클라이언트 도구"   "mosquitto"      "mosquitto-clients"

# Node.js / npm (클라이언트 빌드)
add_dep "node"  "Node.js (클라이언트 빌드)"  "node"  "nodejs"
add_dep "npm"   "npm (패키지 관리)"           "node"  "npm"

# Python + pytest + paho (통합 테스트)
add_dep "python3"  "Python 3"  "python3"  "python3"
add_dep "pip3"     "pip3"      "python3"  "python3-pip"

# ── 의존성 점검 및 설치 ───────────────────────────────────────────────────────
echo ""
info "=== 의존성 점검 시작 ==="
echo ""

MISSING_ANY=false
for i in "${!DEPS_CMD[@]}"; do
  cmd="${DEPS_CMD[$i]}"
  desc="${DEPS_DESC[$i]}"
  macos_pkg="${DEPS_MACOS[$i]}"
  linux_pkg="${DEPS_LINUX[$i]}"

  if command -v "$cmd" &>/dev/null; then
    ok "$desc ($(command -v "$cmd"))"
  else
    MISSING_ANY=true
    if ask_install "$desc"; then
      install_pkg "$desc" "$macos_pkg" "$linux_pkg"
    else
      warn "$desc 건너뜀 — 이후 빌드/실행이 실패할 수 있습니다."
    fi
  fi
done

# ── libmosquitto / libcjson 헤더 존재 여부 확인 (pkg-config) ─────────────────
echo ""
info "=== 라이브러리 헤더 점검 ==="

check_pkgconfig() {
  local pc_name="$1" desc="$2" macos_pkg="$3" linux_pkg="$4"
  if pkg-config --exists "$pc_name" 2>/dev/null; then
    ok "$desc ($(pkg-config --modversion "$pc_name" 2>/dev/null || echo 'found'))"
  else
    MISSING_ANY=true
    if ask_install "$desc 개발 헤더"; then
      install_pkg "$desc 개발 헤더" "$macos_pkg" "$linux_pkg"
    else
      warn "$desc 헤더 건너뜀"
    fi
  fi
}

check_pkgconfig "libmosquitto" "libmosquitto (C 라이브러리)" "mosquitto" "libmosquitto-dev"
check_pkgconfig "libcjson"     "libcjson (JSON 파서)"        "cjson"     "libcjson-dev"

# ── Python 패키지 확인 (pytest, paho-mqtt) ────────────────────────────────────
echo ""
info "=== Python 패키지 점검 ==="

check_py_pkg() {
  local import_name="$1" pip_name="$2" desc="$3"
  if python3 -c "import $import_name" &>/dev/null; then
    ok "$desc"
  else
    MISSING_ANY=true
    if ask_install "$desc (pip)"; then
      info "pip3 install $pip_name ..."
      pip3 install "$pip_name"
      ok "$desc 설치 완료"
    else
      warn "$desc 건너뜀"
    fi
  fi
}

check_py_pkg "pytest"           "pytest"              "pytest"
check_py_pkg "paho.mqtt.client" "paho-mqtt>=2.0"      "paho-mqtt 2.x (통합 테스트)"

# ── mosquitto WebSocket 지원 확인 ────────────────────────────────────────────
echo ""
info "=== Mosquitto WebSocket 지원 확인 ==="
if mosquitto --help 2>&1 | grep -qi "websocket\|ws"; then
  ok "Mosquitto WebSocket 지원 확인됨"
elif pkg-config --libs libmosquitto 2>/dev/null | grep -qi "websocket"; then
  ok "Mosquitto WebSocket 지원 확인됨 (pkg-config)"
else
  # Homebrew mosquitto는 기본적으로 WebSocket 포함
  if [[ "$PLATFORM" == "macos" ]] && brew list mosquitto &>/dev/null; then
    ok "Mosquitto (Homebrew) — WebSocket 지원 포함"
  else
    warn "Mosquitto WebSocket 지원 여부를 자동으로 확인하지 못했습니다."
    warn "mosquitto.conf 의 'protocol websockets' 리스너(9001)가 필요합니다."
    warn "WebSocket 지원 없이 빌드된 경우 클라이언트 연결이 실패할 수 있습니다."
  fi
fi

# ── 빌드 단계 ─────────────────────────────────────────────────────────────────
if [[ "$OPT_BUILD" == true ]]; then
  echo ""
  info "=== Broker 빌드 ==="
  cmake -S broker -B broker/build
  cmake --build broker/build --parallel "$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
  ok "broker/build/core_broker, edge_broker 빌드 완료"

  echo ""
  info "=== Client npm install ==="
  npm install --prefix client
  ok "client/node_modules 설치 완료"
fi

# ── 테스트 단계 ───────────────────────────────────────────────────────────────
if [[ "$OPT_TEST" == true ]]; then
  echo ""
  info "=== ctest 실행 ==="
  ctest --test-dir broker/build --output-on-failure
  ok "ctest 완료"
fi

# ── 완료 요약 ─────────────────────────────────────────────────────────────────
echo ""
info "=== 설치 완료 ==="
if [[ "$MISSING_ANY" == false ]]; then
  ok "모든 의존성이 이미 설치되어 있었습니다."
fi

echo ""
echo "  다음 명령어로 빌드 및 실행할 수 있습니다:"
echo ""
echo "  # Broker 빌드"
echo "  cmake -S broker -B broker/build && cmake --build broker/build"
echo ""
echo "  # Mosquitto 브로커 실행 (별도 터미널)"
echo "  mosquitto -c mosquitto.conf"
echo ""
echo "  # Core Broker 실행"
echo "  broker/build/core_broker 127.0.0.1 1883"
echo ""
echo "  # Edge Broker 실행"
echo "  broker/build/edge_broker 127.0.0.1 1883 127.0.0.1 1883"
echo ""
echo "  # Web Client 개발 서버"
echo "  npm run client:dev"
echo ""
echo "  # 전체 테스트"
echo "  bash install.sh --test"
echo "  python3 -m pytest test/integration/ -v"
