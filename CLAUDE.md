# CLAUDE.md — PokeZ-Android (ZA Recomp para Android)

Instruções para assistentes de IA (e humanos) trabalhando neste clone. O projeto porting é o branch `android-port`
de [ZA Recomp](https://github.com/Hexveil404/PokeZ-ARecomp) (LAS Recomp de Peiikai), publicado em
https://github.com/MrSw4gGuy/PokeZ-Android.

## Visão geral

Port Android (arm64) de Pokémon Legends: Z-A (Switch 1) por recompilação estática: o código do jogo vira C, o SO e a
GPU (NVN) são reimplementados sobre Vulkan. O runtime é em C; o build Android roda no WSL (Ubuntu 22.04) e gera
`ZARecomp-za-debug.apk`.

## Comandos essenciais

```bash
# Runtime Android (rápido; confere erros de compilação)
wsl.exe -d Ubuntu-22.04 -u root -- bash -lc "bash '/mnt/d/Claude/Recomp/Pokemon Legends ZA/PokeZ-ARecomp/tools/linux/android-port/build_android.sh'"

# APK completo (sincroniza clone Windows→WSL e empacota; ~100 s)
wsl.exe -d Ubuntu-22.04 -u root -- bash -lc "bash '/mnt/d/Claude/Recomp/Pokemon Legends ZA/PokeZ-ARecomp/tools/linux/android-port/za_apk.sh'"
# Saída: D:/Claude/Recomp/Pokemon Legends ZA/ZARecomp-za-debug.apk — procurar 'ZA APK OK' no fim.

# Instalar no celular (ADB Wi-Fi; a porta muda a cada sessão)
./platform-tools/adb.exe mdns services          # descobrir a porta
./platform-tools/adb.exe connect 192.168.0.99:<porta>
./platform-tools/adb.exe install -r ZARecomp-za-debug.apk
./platform-tools/adb.exe shell am start -n org.recomp.za/.ZARecompActivity   # NÃO é MainActivity

# Logs e configuração no celular
adb pull /storage/emulated/0/Android/data/org.recomp.za/files/lasrecomp.log .
adb shell "grep -E '^(fps60|fps45|frame_gen|shadow_distance|thermal_profile|za_lod) *=' \
  /storage/emulated/0/Android/data/org.recomp.za/files/larecomp.ini"
```

Teste de GPU: `LARECOMP_VK_PROF=1` e `LARECOMP_GPUPROF_AT=<frame>[,<frame>...]` no `env.txt` do celular; exige
force-stop + relançar (env.txt lido no início). Linhas `[gpuprof]` no log. Tema térmico:
`dumpsys thermalservice` (Status 0 none … 4 critical; SKIN é o sensor da pele).

## Arquitetura do runtime (o que morre onde)

- `runtime/main.c` — boot; `runtime/kernel.c` — threads/fibers do jogo.
- `runtime/hle/` — serviços do Switch refeitos. `framepace.c` é o coração do frame pacing do Z-A:
  `rt_za_frame_cfg()` intercepta o relógio do jogo (patch `tools/za_patches.py`, f_70019e3730): o jogo conta
  vsyncs inteiros por quadro (`max(modo+1, passados)`, unidades fixas de 1/60 s); modo 60 = cfg 0. O Z-A nunca pede
  o evento de vsync do vi.c: o relógio de quadros dele é o `present_pace` de `nvn/objects.c` (preso ao refresh medido
  do display no Android).
- `runtime/nvn/` — a GPU do Switch. `vulkan/exec.c` grava comandos; `vulkan/device.c` cria swapchain
  (pre-rotation no Android) e chama `pace_frame()`; `pace.c` segura o cadência (`hold_cadence`/`hold_30`);
  `output_scale.c` aplica a escala dinâmica; `shaders.c` tem os built-ins (GLSL → SPIR-V na hora).
- `runtime/platform/android.c` — térmico (`plat_thermal_scale`: perfil com/sem cooler em `settings.thermal_profile`;
  `plat_thermal_throttled()`: 1 enquanto a escala térmica < 100%), ADPF perf hints, frame rate hint
  (`ANativeWindow_setFrameRate`).
- `runtime/ui/menu.mm` — menu F1 (opções de vídeo, sombras, Turbo, térmico). `runtime/settings.{c,h}` — INI
  (`larecomp.ini` no celular) e defaults.
- Patches no jogo: `tools/za_patches.py` (endereços da versão 2.0.2; `runtime/game_addresses/`). Tweaks de gráficos
  do jogo: `runtime/hle/za_tweaks.c` (LOD bias, DOF, FXAA, bloom).

## Convenções

- Comentários longos em inglês, explicando o PORQUÊ (histórico e medidas); logs em inglês via `LOG(...)`.
- Opções de menu bilíngues: `TR("English", "Português")`.
- Env vars `LARECOMP_*` lidas com `ENV()` (hle.h); config do usuário: `larecomp.ini`.
- TODO: nunca commitar sem pedir; nunca subir APN/chaves do jogo; o APK é sempre local.

## Estado do projeto (atualizado em 2026-11-07)

- **Sombras (F1 > Vídeo > Distância das sombras)**: Completa / Reduzida / Só próximas / Só personagens / Sem sombras.
  Reduzida = cascata distante a 512×512 copiada para o layer 1 do mapa do jogo (passe de cópia em render, porque a
  Adreno não faz blit no D32 do jogo). Economia medida: ~0,25 ms (~9%) no trecho das cascatas.
- **Perfil térmico (F1 > Vídeo)**: Com cooler / Sem cooler — muda os degraus e a recuperação de
  `plat_thermal_scale` (com cooler 100/85/70/60; sem cooler até 50%, recupera mais devagar).
- **Perfil Turbo (F1 > Vídeo, botão)**: aplica 60 fps com fallback dinâmico, resolução dinâmica do jogo, sombras
  Reduzidas, LOD Desempenho, FG off — e salva o INI.
- **LOD (F1 > Vídeo > Nível de detalhe)**: Do jogo / Alto (bias -1.0) / Desempenho (bias +1.0, via za_tweaks).
- **Pré-compilação de pipelines**: já existia (shader_prep); log mostra cache de 6,4 MB e 0 falhas.
- **ADPF**: já existia (plat_perf_hint + térmico); não duplicar.
- **Modo 45 fps**: REMOVIDO (2026-11-09, PzPace). Nunca passou de 30 (present_pace segura 33 ms; o vi.c nem roda no
  Z-A) e, se chegasse a 45, o mundo andaria 1,5x (passos de 1/60 s). `fps45` em inis antigos é lido e ignorado.
- **Glitch nos olhos** (íris com faixa branca): NÃO é o LOD (persiste com za_lod=0). Aberto: investigar shader/
  textura do olho no backend Vulkan.
- **Throttling**: com Turbo + cena pesada, o telefone entra em status 2/3 mesmo com cooler; o retry do 60 fps agora
  espera a escala térmica recuperar (plat_thermal_throttled). Para cenas pesadas, 30 fps segue sendo o modo seguro.

## Lições de debugging (todas vividas aqui)

1. `dlopen` aceita símbolo indefinido em .so — o crash aparece SÓ ao abrir o app
   (`cannot locate symbol "g_fps45_hold"`). Toda variável `extern` usada por pace.c tem de ser DEFINIDA em
   framepace.c.
2. `GA(GAME_MGR)` antes do jogo montar o manager = ponteiro com lixo não-nulo → SIGSEGV dentro de `hold_30`.
   Qualquer novo acesso à memória do jogo precisa de guard baseado no hook do jogo (`g_za_frame_hook`) ou em
   `g_ga`/`fw` válidos.
3. `LARECOMP_FPS=60` no env.txt FORÇA 60 e DESLIGA o fallback — nunca usar em teste de desempenho do modo dinâmico.
4. Diagnóstico de "resolução baixa": olhar `[thermal] headroom ... render scale X%` + `[scale]` no log — a escala
   térmica multiplica a do DRS; ver também `[post] WxH sgsr -> 1920x1080`.
5. `llvm-addr2line -e <libmain.so merged_native_libs> -f -C <pc>` para backtrace do log de crash; os .o em
   `/root/lasrecomp/build/android/rt/**` dão linha exata (pace.c etc.).
6. O INI do celular é a fonte da verdade do teste (`fps60`, `fps45`, `fps60_lock`, `frame_gen`,
   `shadow_distance`, `za_lod`, `thermal_profile`); conferir antes de qualquer medição.

## Pendências abertas

- 45/40 fps de verdade: só com passos fracionários no relógio do jogo (eval/reports/pz-PzPace.md).
- Glitch nos olhos: shader/textura do olho no backend.
- Na pasta de testes: limpar `files/save` e `env.txt` do celular e restaurar `shadow_resolution = 0.5` no ini
  (o usuário aprovou copiar/apagar saves de teste).
- Do BytArch GTA V (GPL-3.0): usar só IDEIAS, não código (licenças: este projeto MIT, aquele GPL-3.0).
