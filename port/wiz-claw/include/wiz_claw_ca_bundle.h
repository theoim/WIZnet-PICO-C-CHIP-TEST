/**
 * wiz_claw_ca_bundle.h — TLS 서버 인증서 검증용 Root CA 번들
 *
 * 이 파일을 직접 편집하거나, 아래 도구로 자동 생성하세요:
 *
 *   cd port/wiz-claw/tools
 *   python3 gen_ca_bundle.py > ../include/wiz_claw_ca_bundle.h
 *
 * WIZ_CLAW_CA_BUNDLE_PEM이 정의되지 않으면 VERIFY_NONE으로 동작합니다.
 * (개발/테스트 단계에서만 허용)
 *
 * ── PEM 번들 직접 지정 예시 ────────────────────────────────────
 *
 * #define WIZ_CLAW_CA_BUNDLE_PEM \
 *     "-----BEGIN CERTIFICATE-----\n" \
 *     "MIIF....\n"                    \
 *     "-----END CERTIFICATE-----\n"  \
 *     "-----BEGIN CERTIFICATE-----\n" \
 *     "MIIF....\n"                    \
 *     "-----END CERTIFICATE-----\n"
 */
#pragma once

/* WIZ_CLAW_CA_BUNDLE_PEM은 기본적으로 정의하지 않습니다.
 * CA 번들이 있으면 위 예시처럼 이 파일 안에 직접 정의하거나
 * gen_ca_bundle.py로 이 파일 전체를 덮어쓰세요. */
