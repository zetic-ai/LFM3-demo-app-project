# IQ9 비교 GUI 인계 (갱신 2026-11-03 22:45 KST)

## 재개 결과 (사용자가 IQ9가 비어 있는 동안 작업 허용)

- 보드 정상 부팅(remoteproc 5개 running), 다른 프로세스·잠금 없음 확인 후 진행. 작업 후 서버/worker/llama/BUSY 잠금 모두 해제.
- 배포: server.py(`a986a6a` + 아래 수정), sdk_worker 재빌드. 보드 설치본과 로컬 HEAD 동일.
- ZETIC/llama 음성(TTS, 음성 답변, 이미지→음성) 통과. 장애 재현 없음, remoteproc 계속 running. 첫 재시작 직후 `hexrt_lib_open ... 0x1` 1회 일시 실패, 재시도 통과.
- 수정 1: ZETIC worker가 시스템 프롬프트 없이 TTS를 요청해 0.56초 스텁이 나오던 문제. CLI와 같은 지시문을 자동 추가 (TTS 3.68 s).
- 수정 2: llama 영상 첨부가 `Errno 21`로 실패하던 버그. 이제 설치된 llama.cpp(b11946/b12004)가 영상 미지원이라는 명확한 메시지 표시. ZETIC 영상은 Omni/VL 모두 통과.
- Stop/반복 Stop/취소 후 다음 요청: 양 엔진 통과. 런처 브라우저 닫기 → 서버·잠금 정리 통과.
- 남음: 양 엔진 영상 비교(영상 지원 llama 빌드 필요), D1-Omni 오디오→Omni TTS 전환 반복.
- 아래 "현재 상태와 작업 중단" 이하는 재개 전 기록이다.

## 현재 상태와 작업 중단

사용자가 IQ9를 강제종료했다고 알리고, 다른 task가 사용해야 하므로 작업 중단을 지시했다. **기기 접속·복구·추론·자동 재시도 금지.** 새 작업 지시 전에는 재개하지 않는다. 마지막 네트워크 확인에서는 오프라인이었으며, 사용자 강제종료 이후 기기 상태는 확인하지 않았다.

로컬 저장소: `/home/changgeun/workspace/LFM3-demo-app-project-d1`, 브랜치 `feat/iq9-comparison-gui`. 기존 구현 커밋:

- `d0c7803`: GUI, 서버, SDK worker, 설치/실행, 샘플·비교·D1 편집, 테스트.
- `a986a6a`: 중복 Stop 및 로딩 중 취소 보완, lifecycle 테스트.

이 문서와 중단 상태를 별도 로컬 커밋으로 보존한다. **원격 push/PR 없음.** 외부 작업자의 `.vscode/`는 untracked 그대로 보존했다. 다른 문서 작업자의 PR #6과 별도 작업이다.

## 구현한 것

- Ubuntu 로컬 브라우저 GUI + aiohttp + GNU C++ SDK worker. 기존 CLI 유지.
- 자유 입력/다중 턴, 이미지·오디오·장문 샘플과 파일 첨부, 영상 입력 경로, 출력 오디오 스트리밍/재생/다운로드.
- ZETIC/llama.cpp 선택 및 같은 입력을 순차 실행하는 Compare, 양쪽 출력과 TTFT/TPS/오디오 지표, IQ9 과거 참고 그래프의 명확한 구분.
- 기본 LFM3 Omni/ZETIC/context 2048, Prepare와 자동 모델 전환. NPU 모델은 하나씩 실행하고 전환 시 이전 프로세스 종료 후 5초 대기.
- D1 Noul/Choice/Score 질문·선택지 편집 및 확률 표시. llama API의 다중 선택 제한과 Score 의미 차이 표시.
- 자기 BUSY 잠금, cDSP crashed 감지 시 추론 차단, 설치 스크립트/데스크톱 런처.

## 실제 검증 범위

| 범위 | 결과 |
| --- | --- |
| 양 엔진 text / multi-turn / cats image / JFK ASR / long summary | IQ9 통과 |
| 양 엔진 D1 ticket(Noul/Choice/Score) / image / audio | IQ9 통과 |
| GNU worker 빌드 | 호스트 cross 및 IQ9 native 통과 |
| 모델 독립 API/lifecycle | 7/7 통과; 반복 Stop 및 로딩 중 취소 포함 |
| 브라우저 | 샘플/질문 편집/settings/실측 결과 복원/1024×768 버튼 표시 통과 |
| llama CPU 음성 프로토콜 | TTS, 음성 답변, image chat→TTS 통과; 성능 비교에 사용하지 않음 |
| ZETIC 실제 음성 출력 / 양 엔진 NPU 음성 완료 | 미완료 |
| 양 엔진 영상 추론 / 실제 추론 취소 후 정상 복구 / 런처 닫기 정리 | 미완료 |

영상은 같은 프레임을 SDK Video와 llama native input_video로 보내도록 구현했고 변환 픽셀 동일성만 검증했다. 선택 가능한 다른 모델 프로필 전부를 시험한 것은 아니다. core는 기능 확인이며 정식 반복 성능 벤치마크가 아니다.

## 장애 경위: 무엇을 시험했는가

1. llama.cpp HTP의 **D1-Omni 오디오 분류**가 성공했다.
2. 다음 시험인 **ZETIC Omni TTS**로 모델을 전환했다.
3. 음성 생성 전에 SDK 모델 로딩이 `0x72`로 실패했다.
4. 커널 SMMU IRQ thread fault와 cDSP watchdog/crash, `remoteproc0=crashed`를 확인하고 NPU 시험을 중단했다.

**정확한 crash 시점·원인 엔진·트리거는 미확정**이다. TTS 생성 중 장애였다고 단정하면 안 된다. 사용자 승인 후 약 15:00 KST에 `sudo -n reboot`를 실행했으나 후속 SSH timeout/Tailscale offline 상태로 복귀를 확인하지 못했다. 이후 사용자가 강제종료하고 작업 중단을 지시했다.

보드 재부팅 전 자기 GUI/worker/llama/browser와 호스트 테스트 서버·SSH 포워드는 정리했다. 중단 요청 후에는 보드에 접속하지 않았다. 재부팅 전 만든 `~/BUSY_lfm3_comparison_gui`가 남아 있을 수 있으므로 **다음 담당자가 다른 작업의 프로세스/잠금부터 확인하고 stale 여부를 판단**해야 한다. 현재 사용자 task를 방해하며 삭제하거나 서버를 시작하지 않는다.

## 배포 상태와 보존한 것

- 보드 앱: `/home/ubuntu/lfm3-comparison-demo/`; 실행 `iq9/gui/run.sh`, 설정 `iq9/gui/config.json`.
- SDK worker: `iq9/out/sdk_worker`; desktop entry `~/.local/share/applications/lfm3-iq9.desktop`.
- 앱 데이터/로그: `~/.local/share/lfm3-iq9/`; 서버 localhost 8765, 관리 llama 18765.
- **`a986a6a` 보완 코드는 보드에 아직 미배포**. 기기 설치본과 로컬 HEAD가 다르다.
- Omni SDK 비교용 모델: `~/lfm3-comparison-models/lfm3-omni-2b/v73`. 기존 single-NSP 원본을 보존하고 같은 가중치에서 공식 2-NSP pf8 팩을 보드 내부 재생성했다. 공식 SHA256SUMS 22항목 전부 통과.
- 나머지 SDK 모델은 `~/zsdk_rc/models/`, `~/zsdk_d1/models/`; llama build/model은 `~/llamacpp/` 아래 기존 파일 재사용. 상세 프로필은 보드 config와 저장소 `config.example.json` 참고.
- 기존 배포 모델 바이트·SDK 버전 변경 없음, HF 업로드 없음. 설치 앱/모델/실패 증거는 보존했다.

## 재개 시 남은 일 (새 지시를 받은 뒤에만)

1. IQ9 사용권과 BUSY/프로세스/remoteproc 상태 확인. 재부팅은 사용자 승인 없이 하지 않는다.
2. 자기 서버를 중지한 상태에서 후속 코드 배포; 실행 중 스크립트 교체 금지.
3. ZETIC/llama 음성 출력·음성 답변·이미지→음성, 영상 추론, Stop/반복 Stop/취소 후 다음 요청, 실제 런처 닫기 정리를 검증한다.
4. 필요할 때만 D1-Omni 오디오→Omni TTS 전환을 로그와 함께 재확인한다. 장애를 반복시키며 무작정 재부팅하지 않는다.
5. README 검증표를 실제 결과로 갱신하고 로컬 커밋한다. push/PR는 별도 지시 전 금지.

설치/실행/명령은 [README.md](README.md), 테스트는 `tests/test_api.py`, `tests/smoke_live.py`, `tests/ui.cjs`에 있다.

## 증거 위치

호스트 로그/스크린샷 디렉터리:
`/mnt/disks/zeticai_database/cg_research/runs/qcs9075-iq9/lfm3-comparison-gui/20261103/`

- `core.log`: 양 엔진 core PASS.
- `speech.log`, `kernel-fault.log`: 실패와 커널 상태 원본.
- `llama-cpu-protocol.log`: CPU TTS/voice 성공 후 초기 pipeline 실패도 포함.
- `llama-cpu-pipeline.log`: HTTP keepalive 보완 후 image→TTS 성공.
- `verified-comparison.png`, `playground-home.png`, `decision-studio.png`: GUI 확인 화면.

상위 인계 문서:
`/mnt/disks/zeticai_database/cg_research/docs/lfm3-sdk/handoff-codex-demo-16kb-20261102.md`
최상단 최신 상태가 오래된 내용보다 우선한다.
