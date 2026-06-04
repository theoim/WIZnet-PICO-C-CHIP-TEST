#!/usr/bin/env python3
"""
gen_ca_bundle.py — api.openai.com / api.telegram.org Root CA 번들 생성기

사용법:
    cd port/wiz-claw/tools
    python3 gen_ca_bundle.py > ../include/wiz_claw_ca_bundle.h

필요 조건:
    openssl 커맨드라인 도구 (PATH에 있어야 함)
    - Windows: https://slproweb.com/products/Win32OpenSSL.html
    - macOS:   brew install openssl
    - Linux:   apt install openssl

출력:
    wiz_claw_ca_bundle.h — WIZ_CLAW_CA_BUNDLE_PEM 매크로 정의
    (mbedtls_x509_crt_parse가 읽는 PEM 형식 Root CA 묶음)
"""

import subprocess
import sys
import re
import os

TARGETS = [
    ("api.openai.com",    443, "OpenAI"),
    ("api.telegram.org",  443, "Telegram"),
]


def fetch_chain_pems(host: str, port: int) -> list[str]:
    """openssl s_client로 서버 인증서 체인의 모든 PEM을 반환."""
    try:
        result = subprocess.run(
            ["openssl", "s_client",
             "-connect", f"{host}:{port}",
             "-showcerts",
             "-verify_return_error",
             "-servername", host],
            input=b"\n",
            capture_output=True,
            timeout=20,
        )
    except FileNotFoundError:
        print(f"# ERROR: 'openssl' 명령을 찾을 수 없습니다.", file=sys.stderr)
        print(f"# PATH에 openssl이 있는지 확인하세요.", file=sys.stderr)
        sys.exit(1)
    except subprocess.TimeoutExpired:
        print(f"# WARNING: {host}:{port} 연결 타임아웃", file=sys.stderr)
        return []

    stdout = result.stdout.decode(errors="ignore")
    # showcerts 출력에서 PEM 블록 전체 추출
    pems = re.findall(
        r"(-----BEGIN CERTIFICATE-----[^-]+-----END CERTIFICATE-----)",
        stdout,
        re.DOTALL,
    )
    return [p.strip() for p in pems]


def get_root_ca(host: str, port: int, label: str) -> str | None:
    """체인의 마지막 인증서(루트 CA)를 반환. 실패 시 None."""
    pems = fetch_chain_pems(host, port)
    if not pems:
        print(f"# WARNING: {label} ({host}) 체인 추출 실패", file=sys.stderr)
        return None
    root = pems[-1]
    print(f"# INFO: {label} ({host}) — {len(pems)}개 체인 중 루트 CA 추출됨",
          file=sys.stderr)
    return root


def pem_to_c_string(pem: str) -> list[str]:
    """PEM을 C 문자열 리터럴 라인 목록으로 변환."""
    lines = []
    for line in pem.splitlines():
        lines.append(f'    "{line}\\n" \\')
    return lines


def main():
    seen = {}   # pem → label (중복 제거)

    for host, port, label in TARGETS:
        root = get_root_ca(host, port, label)
        if root and root not in seen:
            seen[root] = label

    if not seen:
        print("# ERROR: Root CA를 하나도 가져오지 못했습니다.", file=sys.stderr)
        print("# 인터넷 연결과 openssl PATH를 확인하세요.", file=sys.stderr)
        sys.exit(1)

    # ── 헤더 출력 ──────────────────────────────────────────────
    script_name = os.path.basename(__file__)
    labels = ", ".join(seen.values())

    print("/**")
    print(f" * wiz_claw_ca_bundle.h — Root CA 번들 ({labels})")
    print(f" * {script_name}로 자동 생성됨")
    print(f" * 인터넷 연결 상태가 바뀌거나 인증서가 갱신되면 재생성하세요.")
    print(" */")
    print("#pragma once")
    print()
    print("#ifndef WIZ_CLAW_CA_BUNDLE_PEM")

    all_lines = []
    for pem, label in seen.items():
        all_lines.append(f"    /* {label} Root CA */")
        all_lines.extend(pem_to_c_string(pem))

    if all_lines:
        print("#define WIZ_CLAW_CA_BUNDLE_PEM \\")
        for line in all_lines:
            print(line)
        print('    ""')
    else:
        print("#define WIZ_CLAW_CA_BUNDLE_PEM  NULL")

    print("#endif /* WIZ_CLAW_CA_BUNDLE_PEM */")


if __name__ == "__main__":
    main()
