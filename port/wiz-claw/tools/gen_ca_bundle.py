#!/usr/bin/env python3
"""
gen_ca_bundle.py — Root CA / Intermediate CA 번들 생성기

- leaf cert (CA:FALSE) 자동 제거
- SHA-256 fingerprint 기반 중복 제거 (PEM 바이트 비교보다 정확)

사용법:
    cd port/wiz-claw/tools
    python3 gen_ca_bundle.py > ../include/wiz_claw_ca_bundle.h
"""

import subprocess
import sys
import re
import os

TARGETS = [
    ("api.openai.com",              443, "OpenAI"),
    ("api.telegram.org",            443, "Telegram"),
    ("gateway.ai.cloudflare.com",   443, "Cloudflare"),
]


def _openssl_with_pem(args: list[str], pem: str) -> subprocess.CompletedProcess:
    """임시 파일 경유로 openssl 실행 (Python 3.14 stdin writerthread 호환)."""
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".pem", delete=False, mode="w") as f:
        f.write(pem + "\n")
        fname = f.name
    try:
        return subprocess.run(args + ["-in", fname], capture_output=True, text=True)
    finally:
        os.unlink(fname)


def fetch_chain_pems(host: str, port: int) -> list[str]:
    try:
        result = subprocess.run(
            ["openssl", "s_client",
             "-connect", f"{host}:{port}",
             "-showcerts",
             "-servername", host],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            timeout=20,
        )
    except FileNotFoundError:
        print("# ERROR: 'openssl' 명령을 찾을 수 없습니다.", file=sys.stderr)
        sys.exit(1)
    except subprocess.TimeoutExpired:
        print(f"# WARNING: {host}:{port} 연결 타임아웃", file=sys.stderr)
        return []

    stdout = result.stdout.decode(errors="ignore")
    pems = re.findall(
        r"(-----BEGIN CERTIFICATE-----[^-]+-----END CERTIFICATE-----)",
        stdout,
        re.DOTALL,
    )
    return [p.strip() for p in pems]


def cert_fingerprint(pem: str) -> str:
    r = _openssl_with_pem(["openssl", "x509", "-noout", "-fingerprint", "-sha256"], pem)
    return r.stdout.strip()


def is_ca(pem: str) -> bool:
    """CA:FALSE이면 False (leaf cert). 번들에 넣지 않음."""
    r = _openssl_with_pem(["openssl", "x509", "-noout", "-text"], pem)
    text = r.stdout
    # CA:TRUE 또는 CA: TRUE 확인
    if re.search(r'CA:\s*TRUE', text):
        return True
    # Basic Constraints가 아예 없는 경우 → 구형 root CA → CA로 취급
    if 'Basic Constraints' not in text:
        return True
    return False


def get_ca_pems(host: str, port: int, label: str) -> list[tuple[str, str]]:
    """leaf 제거 + (fingerprint, pem, label) 반환."""
    pems = fetch_chain_pems(host, port)
    if not pems:
        print(f"# WARNING: {label} ({host}) 체인 추출 실패", file=sys.stderr)
        return []

    ca_pems = []
    for pem in pems:
        if is_ca(pem):
            fp = cert_fingerprint(pem)
            ca_pems.append((fp, pem))
        else:
            print(f"# INFO: {label} leaf cert 제외 (CA:FALSE)", file=sys.stderr)

    print(f"# INFO: {label} ({host}) — CA cert {len(ca_pems)}개", file=sys.stderr)
    return ca_pems


def pem_to_c_string(pem: str) -> list[str]:
    lines = []
    for line in pem.splitlines():
        lines.append(f'    "{line}\\n" \\')
    return lines


def main():
    seen: dict[str, str] = {}   # fingerprint → label

    for host, port, label in TARGETS:
        for fp, pem in get_ca_pems(host, port, label):
            if fp not in seen:
                seen[fp] = (label, pem)
            else:
                print(f"# INFO: {label} cert 중복 (이미 {seen[fp][0]}에서 포함)", file=sys.stderr)

    if not seen:
        print("# ERROR: CA cert를 하나도 가져오지 못했습니다.", file=sys.stderr)
        sys.exit(1)

    script_name = os.path.basename(__file__)
    labels = ", ".join(label for label, _ in seen.values())

    print("/**")
    print(f" * wiz_claw_ca_bundle.h — Root/Intermediate CA 번들 ({labels})")
    print(f" * {script_name}로 자동 생성됨")
    print(f" * 인터넷 연결 상태가 바뀌거나 인증서가 갱신되면 재생성하세요.")
    print(" */")
    print("#pragma once")
    print()
    print("#ifndef WIZ_CLAW_CA_BUNDLE_PEM")

    all_lines = []
    for fp, (label, pem) in seen.items():
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
