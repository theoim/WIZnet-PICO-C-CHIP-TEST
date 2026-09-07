# tools — 실기 검증 스크립트

호스트 PC에서 돌린다. 장비는 `opc.tcp://192.168.11.2:4840` 로 가정한다.

```
pip install asyncua
python session_scaling_test.py     # 세션 1~N FreeHeap + 상한 도달 지점
python socket_saturation_test.py   # 소켓 포화 시 거절 동작
SOAK_HOURS=12 python soak_test.py  # 장시간 누수 (시간 누수 / 개폐당 누수 분리)
```

**UAExpert 가 붙어 있으면 소켓 하나를 이미 쓰고 있다.** 세션 수를 셀 때
반드시 포함해야 한다. 2026-09-03 측정에서 이것을 빠뜨려 한도가 3개로
보였다 — 실제로는 UAExpert 1 + 스크립트 3 = 4개였다.

결과는 `docs/memory_budget.md` 에 있다.

전송 계층(다중 listen · 버퍼 크기) 자체를 보는 시험은 별도다 —
`examples/socket_listen_test` 를 장비에 올려서 쓴다.

## soak_test.py 환경변수

| 변수 | 기본값 | 뜻 |
|---|---|---|
| `SOAK_HOURS` | 12 | 시험 시간 |
| `SOAK_CYCLE_S` | 60 | 세션 개폐 주기 |
| `SOAK_REPORT_S` | 1800 | 중간 요약 주기 |
| `SOAK_CSV` | `soak_log.csv` | 로그 경로 |
| `SOAK_URL` | `opc.tcp://192.168.11.2:4840` | 장비 주소 |

**호스트 PC 절전을 꺼야 한다.** 절전에 들어가면 시험이 그 자리에서 끊기고,
장비 쪽에는 FIN 없이 사라진 세션이 남는다. 확인:
`powercfg /q SCHEME_CURRENT SUB_SLEEP STANDBYIDLE` — AC 값이 0이면 사용 안 함.
