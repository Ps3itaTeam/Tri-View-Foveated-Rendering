# Test e strumenti della TAA a supersampling (`d3d12::SuperTAA`)

Tutto quello che serve per verificare il passaggio **senza il gioco** e per analizzare una cattura fatta **nel gioco**.

| File | Cosa fa |
|---|---|
| `resolvers.py` | modello numpy di riferimento: `NewTAA` = la TAA a supersampling, passaggio per passaggio come gli shader; `OldTAA` = la TAA del mod (`FoveaTAA`). Una modifica a uno shader va fatta anche qui, e viceversa. |
| `scene.py` | prima scena sintetica con la soluzione esatta (media d'area di ogni pixel): texture, linee sottili, rete scura, riflessi che compaiono e scompaiono, bordo obliquo, zone plate, fuoco, ombra che si muove senza motion vector, luce che si accende, metallo spazzolato, un oggetto che si muove; camera ferma / tremolio della testa / panoramica / rotazione. `python3 scene.py data` (2-3 minuti) |
| `scene2.py` | seconda scena (fix77): quello che lascia **fantasmi**. Testo che scorre sulla sua superficie senza motion vector, due "sedie" (sbarre con vuoti) e due fili più sottili di un pixel che si muovono rispetto alla parete (parallasse lenta e meno lenta), ombra stocastica come la rende il motore (ogni pixel a ogni frame o in luce o in ombra), riflesso che scivola, lampada che cambia ogni 3 frame, scintille, linee sottili. `python3 scene2.py data` (2 minuti) |
| `evaluate.py`, `sweep.py` | prima scena: misure contro la soluzione esatta per zona (errore, sfarfallio dell'errore, dettaglio) e confronto di varianti: `python3 evaluate.py data tremor,pan`, `python3 sweep.py sweep_example.json` (serve scipy) |
| `evaluate2.py` | seconda scena: errore sul testo prima / mentre scorre / dopo, sulla parete attorno alle sedie, sui fili, sull'ombra stocastica, ecc.: `python3 evaluate2.py data tremor,pan` |
| `gpu/supertaa_test.cpp` | il test GPU: compila gli shader con il vero `D3DCompile` (`cs_5_0`), controlla che i blocchi di costanti degli shader coincidano con le struct C++, esegue la classe del passaggio su una sequenza e scrive ogni immagine; prova anche le viste di debug, il confronto a metà, il cambio di scala della storia con liste ancora in coda, l'assenza della profondità, `TextureDump` e una cattura completa con `SuperTAACapture` (formati del gioco, TAA del mod accanto). `gpu/build.sh` |
| `make_case.py`, `onestep.py`, `compare_case.py`, `run_cases.sh` | scrivono un caso, eseguono il test GPU e confrontano il risultato degli shader con il modello (14 combinazioni di impostazioni sulla prima scena, 4 sulla seconda attorno al testo che scorre, 4 con **mosche** sintetiche (`flies=N`: piccole cose vicine che si spostano di parecchi pixel per frame senza che i motion vector lo dicano), gli altri percorsi, la cattura) |
| `capture_io.py`, `analyze_capture.py` | leggono una cattura del gioco (`quadview_dump/supertaa_captureNN`) e: la rieseguono con il modello e la confrontano con le immagini del gioco; controllano jitter e motion vector contro lo spostamento reale dell'immagine; misurano dettaglio e sfarfallio di TAA a supersampling, TAA del mod e frame grezzi, e **quanto resta dei frame precedenti dove l'immagine è cambiata** (fantasmi); scrivono le immagini (PNG); con `--set` riprovano gli stessi frame con altre impostazioni. Una cattura scritta da fix76 o da fix77 (altre statistiche, altre costanti) non viene confrontata: i suoi frame passano per il modello di questa versione, con le impostazioni di questa versione ("this version"). Una cattura di fix78 (o con *no trails behind things that jump* spento) viene confrontata così com'è, e in più rieseguita con quel controllo acceso ("this version") |
| `syntax_check/` | controllo di sintassi e tipi dei sorgenti del mod con il compilatore incrociato MinGW (senza MSVC) |
| `gpu/periphery_test.cpp`, `check_periphery.py`, `extract_shader.py` | fix80, la periferia: la riproiezione con 2 x 2 pixel per raggio (`d3d12::MonoPeriphery`, `Params::ss` 2) confrontata con quella di prima, lo shader del passaggio dell'occhio sulla periferia normale e doppia (gruppi da 32 x 32 e da 16 x 16) confrontato con la formula dell'ingrandimento; `periphery_test.exe compile ...` compila un punto d'ingresso di uno shader con il vero `D3DCompile` e ne scrive il codice (per confrontare due versioni); `extract_shader.py` tira fuori il testo di uno shader da un sorgente C++ e ne controlla le dimensioni dei pezzi |
| `periphery/psim.py`, `pchain.py`, `pcap.py` | fix80: simulazione della catena della periferia (anti-aliasing della vista larga → parte dell'occhio → ingrandimento) sulla scena sintetica vista alla densità dell'occhio, e sulle catture del gioco rifatte come vista larga; vedi sotto |
| `periphery/stable_depth.py`, `periphery/reproject_sequence.py` | fix81: il riferimento di `d3d12::StableDepth` (le distanze della vista larga rese ferme nel tempo per la riproiezione) e la catena della periferia su una cattura del gioco **con la riproiezione vera** (`gpu/periphery_test.exe seq`): profondità del frame, senza jitter, del modello, del passaggio del gioco; vedi *fix81* |
| `ui_inside_test.py` | fix82: i menu del gioco lasciati nell'immagine della vista della fovea per la TAA a supersampling: i frame veri di una cattura (fix81: i primi 8, attraverso il modello da una storia vuota; fix82: l'uscita del gioco) contro il pixel del frame letto dove il jitter l'ha messo, nelle parti chiare. (Sostituisce `ui_menu_test.py` di fix81: il suo menu inventato su una scena scura non mostrava il difetto visto nel gioco.) |
| `periphery_stability.py` | fix82: la vista larga di una cattura con la catena attraverso il modello con più valori di stabilità (sfarfallio a più scale, dettaglio, distanza dall'immagine del gioco) |
| `periphery_history.py` | fix83: la vista larga di una cattura con la catena attraverso il modello con più valori del filtro della storia della periferia (`quadview::Settings::periphery_history_sharpness`): sfarfallio a più scale, dettaglio, quanto un frame ha sopra il proprio passa-basso (i contorni ondulati), distanza dall'immagine del gioco; `--sheet` mette i risultati fianco a fianco |
| `periphery/outline_test.py` | fix84: il contorno di una cosa vicina attraverso `d3d12::StableDepth` e `d3d12::MonoPeriphery` (programma di prova sulla GPU) contro l'immagine vera dell'occhio, in una scena inventata di cui si conosce la risposta: errore, pixel che cambiano lato senza motivo, posto del contorno nel tempo; varianti `game` (una superficie per raggio) e `soft` (`soft_outlines`); `--speed` il movimento in texel per frame |
| `periphery/dump_pair.py` | fix85: i due frame di un dump del gioco con la catena intera della periferia ("Dumps: the periphery's chain whole") attraverso `d3d12::MonoPeriphery` del programma di prova, con lo stable depth del gioco (`seq`, chiave `given 1`): cosa cambia da un frame all'altro vicino ai gradini di profondità e altrove, i pixel che cambiano di oltre il 3 / 6 / 12 %, i pixel isolati, contro l'immagine larga da sola. `--check`: distanza dall'immagine del gioco (0,00003 con lo stesso build). |
| `ui_steady_test.py` | fix86: `d3d12::UiSteady` (l'interfaccia della vista larga raccolta nel tempo per la periferia) su un menu sintetico con la risposta nota, attraverso il passaggio stesso (`periphery_test.exe uisteady`, con `GuiMap`): errore e cambiamento da un frame all'altro dell'immagine del frame come letta fino a fix85 e di quella raccolta, a più velocità della testa; uguaglianza delle copie lontano dal menu; blocchi segnati di una linea sottile senza sfondo; in quanti frame sfuma una scritta che cambia. `--srgb`: il colore come immagine sRGB a 8 bit. |
| `analyze_chain.py` | fix81: legge la catena del frame che la cattura del gioco registra accanto alla TAA della fovea (`chain.txt`: vista larga, distanze, periferia dell'occhio, immagine finale, immagini dell'interfaccia) e dice a quale stadio nasce lo sfarfallio fix82: ogni stadio confrontato dove tutti i suoi frame si coprono, ogni frame al suo angolo (la parte registrata da fix81 seguiva lo sguardo: i suoi spostamenti contavano come sfarfallio), due scale, e l'immagine finita accanto alla fovea (`eye_out_side`). |

## Eseguire

```
python3 scene.py data                       # le sequenze (una volta)
python3 scene2.py data
gpu/build.sh                                # serve g++-mingw-w64-x86-64
RUN="xvfb-run -a wine" ./run_cases.sh       # Linux: wine + vkd3d + un driver Vulkan (basta lavapipe) e, accanto all'exe,
                                            # il d3dcompiler_47.dll di Microsoft (quello di wine non è FXC)
```

Risultato atteso (fix80, 17.11.2026; i 22 casi di fix79 danno gli stessi numeri di fix79): per ogni caso `pixels off by more than 0.02: 0` (nei casi `ghost_s1` e
`ghost_follow2` 2 e 6 su 89856: una soglia a conteggio che scatta un frame prima o dopo), `pixels following a change`
maggiore di 0 tranne che in `nofollow` e `protected` fra 20 000 e 42 000 tranne che in `noprotect` e `ghost_noprotect`
(0); `where a thing arrived: 0 (marked: 0)` in tutti i casi senza mosche (anche sulla scena dei fantasmi, con le sue
sedie e i suoi fili che si muovono), 468, 468 e 654 (marcati circa 3000, 3000 e 4000) nei casi `flies`, `flies_s1` e
`ghost_flies`, 0 in `flies_off`; `paths: all ok`; per la cattura `-> -mv + dj` (il jitter ha il segno che il passaggio si
aspetta) e nel replay nessun pixel oltre 0,02; per la seconda cattura, con le mosche dentro, di nuovo nessun pixel oltre
0,02 e `things that arrived ... in 13 of 16 frames` (lì il replay può seguire i pixel solo per una decina di frame: il
controllo legge il frame prima fino a 4 pixel più in là, e la parte catturata è di 96 pixel). La riga `supersampling TAA against the mod's TAA` di quella cattura (linee sottili e riflessi
sintetici, TAA del mod con feedback 0,90): `detail +97 %, fine detail +144 %, flicker -25 %` (fix77, senza protezione:
+14 %, +31 %, +16 %).

I casi nuovi di fix80 (l'istanza della periferia, con le storie di una vista sola): `wide2x`, `wide2x_flies`,
`ghost_wide2x` (immagine doppia: `out_ratio=2 scale=1 sharpen=0`) e `wide1x` (immagine della dimensione della vista,
riduzione Lanczos 2): `pixels off by more than 0.02: 0`. Poi le righe `periphery:`: le dimensioni dei pezzi dello shader
della riproiezione (al massimo 16380 byte l'uno), per ogni occhio `faster search against the original pass at ss 1:
largest difference 0`, `ss 2: the four pixels of a ray off its own place by at most 4.3e-05 ...; pixels off by more than
0.02: 0; alpha differing: 0 -> ok`, quattro righe `eye pass, periphery ... -> ok` (differenza massima dalla formula
circa 2e-07) e `periphery 2 x 2 per ray: all ok`. La classe `d3d12::EyeCompose` non gira con vkd3d 1.2 su lavapipe (la
sua root signature chiede più descriptor set di quanti il driver ne dà): il test esegue il suo shader con una root
signature equivalente.

`python3 evaluate2.py data pan` (TAA del mod con feedback 0,78, TAA a supersampling 2x, la stessa senza protezione = quello
che faceva fix77, e 1x; le colonne nella testa del file):

```
variant        scroll: before while  1-6    7-14  | ringA  ringB  | wire+ rm  fl   wire- rm  fl  | dither rm fl  | slide  blink  | sparkle rm fl | lines rm  fl  | walls rm  fl
old78          0.0630 0.1231 0.0906 0.0712 | 0.0287 0.0287 | 0.0775 0.0134  0.0993 0.0278 | 0.0454 0.0086 | 0.0171 0.0078 | 0.0696 0.0124 | 0.1510 0.0274 | 0.0173 0.0034
s2             0.0414 0.1536 0.0944 0.0546 | 0.0327 0.0268 | 0.0825 0.0085  0.0883 0.0182 | 0.0338 0.0021 | 0.0163 0.0060 | 0.0591 0.0100 | 0.0995 0.0223 | 0.0111 0.0019
s2_noprotect   0.0403 0.1380 0.0877 0.0535 | 0.0320 0.0300 | 0.0822 0.0088  0.0960 0.0200 | 0.0374 0.0038 | 0.0162 0.0060 | 0.0698 0.0109 | 0.1513 0.0247 | 0.0181 0.0023
s1             0.0607 0.1439 0.0994 0.0741 | 0.0324 0.0297 | 0.0786 0.0122  0.0896 0.0197 | 0.0346 0.0033 | 0.0137 0.0044 | 0.0567 0.0104 | 0.1181 0.0221 | 0.0152 0.0019
```

(fix76 sulla stessa scena: `scroll` 0.0408 0.1621 0.1550 0.1217, `ringA` 0.0624, `slide` 0.0210.)

## Analizzare una cattura del gioco

```
python3 analyze_capture.py <cartella supertaa_captureNN>
python3 analyze_capture.py <cartella> --set af_relax=1.0 w_max=8  # e se le impostazioni fossero altre?
python3 analyze_capture.py <cartella> --bits 8                   # filtro bilineare con 8 bit di frazione (GPU reali)
```

Scrive `analysis/analysis.txt` e le immagini nella cartella della cattura (`--out` per un'altra). Come leggere i numeri:

* **replay**: "off by more than 0.02" vicino a 0 % = il passaggio nel gioco fa quello che fa il modello. Il primo frame
  parte dallo stato catturato ed è confrontato immagine per immagine (colore lineare, guida, box, statistiche, storia).
  Per una cattura di fix76 o di fix77 questa parte manca: il modello di questa versione parte dalla storia catturata
  (e da statistiche vuote) e le righe "this version" dicono cosa ne fa (misure dal frame 6, dal 16 con 48 frame o più:
  fino a lì si sta ancora assestando).
* **inputs**: "picture moved by -mv + dj" deve avere il residuo più piccolo; se vince `-mv - dj` il jitter arriva con il
  segno sbagliato, se vince `-mv` i motion vector contengono già il jitter.
* **flicker**: sfarfallio dell'immagine smussata su ~4 pixel lungo il movimento. Contro lo sfarfallio vero della
  simulazione va d'accordo su linee sottili, reti, riflessi e bordi; su trame fitte e regolari l'immagine che ne conserva
  di più segna fino a 1,5 volte di più anche se il suo errore sfarfalla meno. Va letto insieme a **detail**. Un fantasma
  è fermo e ha contorni: abbassa "flicker" e alza "detail". Per questo c'è la misura sotto.
* **what is left of the picture of k frames before where it has changed since**: 0 = niente, 1 = l'uscita mostra ancora
  l'immagine vecchia. Conta i punti dove l'immagine, smussata su ~7 pixel, è cambiata lungo i motion vector (qualcosa
  che scorre senza motion vector, una parete scoperta, una luce). Sul testo del mangianastri della prima prova: fix76
  0,42 / 0,25 / 0,15 a k = 2 / 4 / 8, TAA del mod 0,20 / 0,05 / 0,00, fix77 0,19 / 0,06 / 0,01. Dove a cambiare sono
  ombre stocastiche a chiazze un numero più alto è la media che si vuole: va letto dove qualcosa è cambiato davvero.
* **following a change**, **protected**, **released** / `follow_last_frame.png`: dove il controllo dei cambiamenti ha
  tolto peso alla storia, dove la storia è protetta (verde) e dove la protezione è rilasciata (rosso), il rumore che il
  controllo crede, e quanto si sparpagliano i campioni (quello da cui la protezione dipende).
* **things that arrived**: in quanti frame e su quanti pixel il passaggio ha riconosciuto una cosa che un frame prima non
  c'era (una mosca: si muove, i suoi motion vector no), e quanti pixel sono "marcati" (lì e attorno, per qualche frame,
  niente protezione e statistiche ferme). Su una scena senza mosche deve restare a 0. Con una cattura di fix78 la riga
  e la colonna "this version" mostrano gli stessi frame con questo controllo: le scie a puntini dietro le mosche non ci
  sono più, e il resto dell'immagine è lo stesso. (La misura "flicker" attorno a una mosca sale: senza scia una mosca
  che salta di 5 pixel per frame compare e scompare, con la scia sfumava. È la scia che non si vuole.)
* con `--set`: la riga e la colonna "what if" sono gli stessi frame con le impostazioni cambiate (chiavi: quelle di
  `NewTAA.DEFAULTS` in `resolvers.py`, per esempio `w_max` = Stability, `clip_gamma` = Clip range, `w_short` =
  History kept where the picture changed, `protect` (0 / 1), `af_relax` / `af_wins` / `af_mem` = Steady detail: range /
  spark limit / longer memory, `arrive` (0 / 1) = no trails behind things that jump, `sharpen`, `hist_extra` = History sharpness), con "what if against the mod's TAA".

## Controllo di sintassi senza MSVC

```
mkdir deps && cd deps && for r in gabime/spdlog g-truc/glm ocornut/imgui cursey/kananlib bitdefender/bddisasm cursey/safetyhook \
  asmjit/asmjit TsudaKageyu/minhook microsoft/DirectXTK12 microsoft/DirectXMath microsoft/DirectX-Headers KhronosGroup/OpenXR-SDK; do
  git clone --depth 1 https://github.com/$r; done; cd ..
syntax_check/sync.sh deps work
syntax_check/check.sh deps work src/mods/TemporalUpscaler.cpp 2>&1 | grep error
```

Gli errori che restano e **non** sono del mod (l'ambiente non è MSVC) sono elencati in testa a `check.sh`.

## La periferia (fix80)

```
python3 periphery/psim.py work/periphery still,tremor,pan,turn 96 <capture.txt di una cattura>   # le sequenze (una volta)
python3 periphery/pchain.py work/periphery tremor raw,light90,light97,super1x,super2x,ideal1x,ideal2x work/periphery/sim
python3 periphery/pcap.py <cartella supertaa_captureNN> out light90,light97,super1x,super2x      # su una cattura del gioco
```

`psim.py`: la scena di `scene.py` vista alla densità dell'immagine dell'occhio e la vista larga che la campiona con un
quarto della densità (jitter del motore dall'intestazione di una cattura, oppure `halton64`). `pchain.py`: le catene
(`light90`, `light97` = Light TAA; `super1x` = TAA a supersampling con l'immagine della dimensione della vista larga;
`super2x` = con l'immagine doppia; `ideal1x`, `ideal2x` = l'immagine esatta mediata sui pixel di un'immagine di quella
dimensione; `raw` = il frame com'è; `super1x_np`, `super2x_np` = senza la protezione) e le misure contro l'immagine
esatta, per gruppo di zone: `detA/B/C` = dettaglio tenuto nelle bande di 4-8, 8-16, 16-32 pixel dell'occhio, `fl1/fl3`
= sfarfallio dell'errore filtrato su 1 / 3 pixel dell'occhio. `pcap.py`: lo stesso su una cattura (la vista larga
rifatta dai frame della fovea), contro l'immagine della fovea.

## fix81: periferia ferma, menu, catena della cattura

`gpu/build.sh` costruisce anche questi modi di `periphery_test.exe`:

* `seq <dir>`: una sequenza di frame attraverso la riproiezione del gioco (`d3d12::MonoPeriphery`), con `stable 1` prima
  attraverso `d3d12::StableDepth`;
* `uiclean <dir>`: frame attraverso `d3d12::UiClean`, vista della fovea o vista larga.

Li usano (serve una cattura del gioco, `RUN` come per `run_cases.sh`):

```
python3 periphery/reproject_sequence.py <cartella supertaa_captureNN> work/seq none,raw,centre,model,game --frames=48
python3 analyze_chain.py <cartella supertaa_captureNN di fix81 o fix82> --sheet=work/chain
python3 ui_inside_test.py <cartella supertaa_captureNN con un menu> --sheet=work/menu.png
python3 periphery_stability.py <cartella supertaa_captureNN con la catena> --values=5,10,16
python3 periphery_history.py <cartella supertaa_captureNN con la catena> --values=0.5,0 --sheet=work/history.png
GPU_DIR=<cartella con periphery_test.exe> python3 periphery/outline_test.py work --frames=64 --speed=0.02,0.01 --variants=game,soft
GPU_DIR=<cartella con periphery_test.exe> python3 periphery/dump_pair.py work <dump01> <dump02> ... --check
GPU_DIR=<cartella con periphery_test.exe> python3 ui_steady_test.py work --weight_max=4 --sheet=work/ui_steady.png
```

Risultati attesi sulla cattura 01 del 17.11.2026 (cucina di RE7, 48 frame):

* `reproject_sequence.py`: sfarfallio x1000 ai gradini di profondità `none 0.987 0.503`, `raw 2.829 1.898`,
  `centre 1.631 0.962`, `model 1.305 0.763`, `game 1.282 0.751`; la riga `the game's StableDepth against the model`
  con circa 0,4 % dei pixel oltre l'1 % (scelte diverse ai pareggi);
* `analyze_chain.py` sulle catture di fix81 del 17.11.2026 16:0x (ogni frame al suo angolo), cattura 03: `wide_color
  30.843 / 18.365`, `wide_taa 1.050 / 0.541`, `eye_periphery 1.186 / 0.683`;
* `ui_inside_test.py` sulle stesse: 01 `0.92 % ... detail kept 89 %`, 06 `0.91 % / 92 %`, 02 `1.27 % / 78 %`,
  05 `3.45 % / 78 %`, 04 `3.40 % / 59 %`;
* `periphery_stability.py`, cattura 03: `the game 2.876 1.321 0.676 0.367`, `model, 5 2.840 1.297 0.669 0.365 | 0.0215 |
  0.0018`, `model, 10 2.509 1.083 0.493 0.259 | 0.0213`.
* `periphery_history.py`, cattura 02 del 17.11.2026 19:2x (fix82): `the game 3.030 1.495 0.670 0.326 | 0.0277 | 0.0317`,
  `model, 0.5 2.841 1.411 0.653 0.322 | 0.0265 | 0.0305 | 0.0046`, `model, 0 1.765 1.053 0.554 0.272 | 0.0220 | 0.0223`.
* `periphery/outline_test.py --frames=64 --speed=0.02,0.01` (fix84): `game ... changing sides per frame without cause 0.019`,
  `soft ... 0.022`, `truth ... 0.022`; con lo `StableDepth` di fix83: `game 0.384`, `soft 0.157`.
* `periphery/dump_pair.py` sui 7 dump del 18.11.2026 16:5x (fix85): `MEAN: 1.138, 0.607 (0.881, 0.626) | 1366 / 362 / 83 (267 / 40 / 1) | 320`;
  con il build di fix84 `1.257, 0.606 | 1962 / 693 / 204 | 407`, con `--soft=0` `1.390, 0.626 | 2261 / 707 / 192 | 801`.
* `ui_steady_test.py --weight_max=4` (fix86), nel menu, errore e cambiamento ×1000 del colore, frame / raccolta: velocità 0 `31.0 44.4 | 6.0 4.2`,
  0,1 `31.0 44.8 | 10.7 4.5`, 0,6 `31.0 41.1 | 15.3 4.9`, 3 `31.2 32.7 | 16.7 15.4`; `0 texels`; la linea sola `25 %` / `100 %`; la scritta che cambia
  sotto un decimo dopo 18 frame (peso 6: oltre 20; peso 3: 14, cambiamento 5,4).
* `periphery/reproject_sequence.py <cattura 02 del 17.11 19:2x> work none,game,soft --frames=48` (fix84): vicino ai salti di
  distanza `none 0.732 0.384`, `game 1.232 0.891`, `soft 1.003 0.635`; altrove `0.496`, `0.491`, `0.490`.

`run_cases.sh` scrive anche, nella cattura "come il gioco" (`work/case_game/capture`), le immagini aggiunte con
`SuperTAACapture::extra` e `chain.txt`: `python3 analyze_chain.py work/case_game/capture` deve elencare 16 frame per
ognuna e uno sfarfallio di `wide_color` molto più alto di quello degli altri stadi (lì sono la stessa immagine).

La statistica dello sparpagliamento con il limite per un campione solo (`af_clip`, `af_clip_floor` in `NewTAA` e in
`SuperTAA::Settings`): `analyze_capture.py` la applica come "this version" alle catture di fix79 e fix80 (che non
l'avevano) e la legge dalle costanti di quelle di fix81.


## fix94: l'interfaccia della periferia raccolta solo attorno a sé (`ui_around/`)

`d3d12::UiSteady` con `Params::only_around` contro l'immagine intera, sul passaggio vero (`gpu/periphery_test.exe
uisteady`, chiavi nuove in `steady.txt`: `around`, `around_from`, `around_to`, `around_hold`, `reset_at`).
`ui_around/make.py` fa una sequenza (scena che si muove, pezzi di HUD che compaiono e spariscono), `ui_around/compare.py`
confronta due esecuzioni (`exact`: stessi byte; `marked`: nei blocchi marcati e attorno), `ui_around/run.sh` fa tutto
(velocità 0 / 0,6 / 3 / 9 pixel per frame, accensione e spegnimento a metà, reset). Risultati del 19.11.2026 in testa a
`run.sh`.

## fix95: il passaggio dell'occhio con l'interfaccia

`gpu/periphery_test.exe <out dir> <testo dello shader dell'occhio>` scrive ora anche `eyeui<ss>_t<gruppo>_L.bin`: lo
stesso passaggio con l'interfaccia della vista larga su ogni pixel (immagini fatte dal programma, vedi
`run_eye_shader`). Per provare una modifica dello shader che non deve cambiare l'immagine: due esecuzioni con il testo
di prima e di dopo (`extract_shader.py`), poi `cmp` dei file `eye*.bin` (uscita in virgola mobile a 32 bit).

## fix99: il passaggio dell'occhio con la mappa del GUI, le ancore e un fascio

`gpu/periphery_test.exe <out dir> <testo dello shader dell'occhio> [map|map1]` scrive anche `eyemap<ss>_t<gruppo>_L.bin`:
l'interfaccia della vista larga con la mappa del GUI (blocchi segnati dove le due copie differiscono entro 2 texel), il
test del riquadro acceso, due ancore rotonde (una sulla scritta, una dove non c'è niente) e un fascio con il posto
adattato nella riga in più della mappa. `map` = solo questo caso (confronto veloce di due testi dello shader), `map1` =
solo con l'immagine della periferia 1x. fix98 contro fix99: gli stessi byte; con uno dei tre salti di `centre_ui`
rovesciato apposta l'immagine cambia.

## fix105: il test dei riquadri in un passaggio suo (`ui_tiles`)

`gpu/periphery_test.exe <out dir> <testo dello shader dell'occhio> map1 tilepass`: il caso della mappa del GUI con i
bit dei riquadri presi dal passaggio `ui_tiles` dello stesso testo, come fa `d3d12::EyeCompose::dispatch` (flag
67108864, misura del riquadro in `g_loops.zw`, immagine R32_UINT in u7). `eyemap*.bin` deve avere gli stessi byte del
caso senza quarto argomento e del testo di fix104. `tilepass_blank` è il controllo: l'occhio legge i bit ma il
passaggio non gira, e l'immagine deve cambiare (fix105: 191 981 / 244 463 pixel su 933 120).

fix106: con `tilepass` il passaggio dell'occhio è compilato con la macro `TILE_BITS` e `ui_tiles` con
`TILE_PASS_ENTRY` (senza macro il testo non dichiara nemmeno `g_tiles`). Per confrontare il bytecode di due testi:
`gpu/periphery_test.exe compile <testo> <entry> <file> [MACRO | MACRO=VALORE ...]`, poi `sha256sum` dei file (fix106
senza macro contro fix104: uguali per `main` 16 x 16 e 32 x 32, `measure`, `gain`, `blocks`, `match`, `light_blur`).

fix108: `tilepass_loop` come `tilepass`, con il passaggio dell'occhio compilato anche con `UI_LOOP` (le chiamate
dell'interfaccia dietro un ciclo di 0 o 1 giri): stessi byte di `eyemap*.bin`.

fix111: `tilepass_split` fa l'immagine dell'occhio in due passaggi come la mod (testo con `UI_NEVER`, poi i riquadri
segnati con il testo `UI_MARKED`): stessi byte di `eyemap*.bin`.

fix112: `tilepass_split2` fa lo stesso con il primo passaggio che lascia i riquadri segnati (`UI_UNMARKED`) e senza
barriera tra i due; `tilepass_split2_first` solo quel primo passaggio (i riquadri segnati devono mancare: nel caso
`map1` il 62 % dell'immagine a 16 x 16, il 69 % a 32 x 32). Stessi byte di `out_old` con `tilepass_split2`.

fix113: `tilepass_split3` registra il passaggio dei riquadri segnati prima di quello dell'immagine dell'occhio (l'ordine
della mod da fix113), senza barriera. Stessi byte di `out_old`.

fix115: dopo il quinto argomento `ui_test1` … `ui_test3` compila il testo dei riquadri segnati con quel `UI_TEST`,
`same_depth` mette i primi due ancorati alla stessa profondità con aree che si sovrappongono (il caso che `UI_TEST` 2
cambia nelle letture). `tilepass_split ui_test2` e `tilepass_split same_depth ui_test2` devono dare gli stessi byte di
`tilepass_split` e `tilepass_split same_depth`.

fix117: `marked8` compila il testo dei riquadri segnati a gruppi di 8 x 8 pixel con `TILE_BITS_SHIFT` (1 con i riquadri
da 16, 2 con quelli da 32: il gruppo legge i bit del riquadro in cui sta) e lo lancia su quei gruppi.
`tilepass_split ui_test2 marked8` e `tilepass_split same_depth ui_test2 marked8` devono dare gli stessi byte delle
corse senza `marked8`. Il controllo `marked8_wrong` (i gruppi piccoli leggono i bit al proprio posto invece che a
quello del riquadro grande) deve dare un'immagine diversa.

fix118: `uiclean <dir>` con `map 1` in `ui.txt` (solo vista larga) rifà ogni frame con `d3d12::GuiMap` fatta prima e data
al passaggio (che allora, in un frame senza interfaccia, esce prima di leggere le due copie): `p%03d.bin` deve avere gli
stessi byte di `o%03d.bin` (stampato per frame). `map 2` è il controllo: il passaggio riceve un timbro che non è quello
del frame, quindi i frame CON interfaccia devono uscire diversi. I frame di prova (640 x 360: la mappa scrive il texel
dell'intero frame solo se è più larga di 64 blocchi) li fa uno script di poche righe: frame senza interfaccia, con un
pannello traslucido e una linea, con un solo texel diverso, con un pannello alle ultime colonne e righe.

fix120: `plain` compila il passaggio dei segnati senza le chiamate dell'interfaccia (la variante 10 della mod: l'immagine
è quella del primo passaggio), `direct` lo stesso con `MARKED_DIRECT` (variante 11: ogni thread fa il suo pixel da solo):
le due immagini devono essere uguali a meno dell'ultima cifra del float (misurato: al massimo 3e-7). `uiclean` con
`map 3` è il controllo dell'uscita per blocco: la mappa è fatta dalle copie del frame prima, quindi un frame con
l'interfaccia altrove deve uscire diverso lì anche se il texel dell'intero frame è timbrato.

fix121: `over` compila il passaggio dei segnati con `MARKED_OVER` (il testo 12 della mod: l'interfaccia sopra il pixel
scritto dal primo passaggio). Nell'harness l'immagine dell'occhio è float, quindi il pixel salvato è la scena esatta e
`tilepass_split ui_test2 over` deve dare l'immagine di `tilepass_split ui_test2` a meno dell'ultima cifra del float, e
nessun pixel diverso dove la catena intera non mette interfaccia (confronto con `plain`).

fix110: `gpu/periphery_test.exe cachetest <cartella>` prepara il passaggio dell'occhio della mod con la cartella data per
il codice compilato (`d3d12::set_shader_cache_dir`); lanciato due volte, la seconda deve dire `0 compiled` e finire in
pochi secondi. I test dei file senza GPU: `tests/shader_file_cache_test.cpp`.

## fix100: la TAA leggera calma della periferia (`periphery/calm.py`, `calm/`)

```
python3 periphery/psim.py work/periphery still,tremor,pan,turn 96 <capture.txt>                 # le sequenze (una volta)
python3 periphery/pchain.py work/periphery tremor light97,calm,ship1x work/periphery/sim        # le misure del modello
python3 calm/make.py work/periphery pan work/calm_pan 40 1 1 16                                 # ingressi per la GPU
gpu/periphery_test.exe calm <work/calm_pan>                                                     # d3d12::LightTAA, 40 frame
python3 calm/compare.py work/periphery pan work/calm_pan                                        # GPU contro modello
python3 calm/compare.py - - work/calm_pan work/calm_pan_altro                                   # due esecuzioni: stessi byte?
```

`periphery/calm.py`: il modello di riferimento (`CalmTAA`, i valori sono quelli dello shader). `pchain.py` ha le
varianti `calm` e `ship1x` (la supersampling 1x con i valori della periferia da fix82 / fix83). `calm/make.py
<sequenze> <tipo> <dir> [frame] [calm 0/1] [gruppi 0/1] [16/32]`; con `calm 0` è il Light TAA di prima (per
confrontare i byte di due build: l'eseguibile costruito contro un albero prima di fix100 si compila con `-DNO_CALM`).
`compare.py` taglia a 16 bit verso zero quello che il modello conserva, come scrive le texture la GPU software.

## fix101: i cambi della guardia senza far ripartire le storie

`gpu/supertaa_test.exe` con `paths=1` (vedi `run_cases.sh`): caso d2, l'immagine cambia dimensione e la storia no
(immagine doppia con `scale` 1 <-> della dimensione della vista con `scale` 2): gli stessi byte di un'istanza che ha
sempre fatto l'altra immagine. `calm/make.py ... <feedback o 0> <fresh_at>`: nel frame `fresh_at` le statistiche della
TAA calma partono da zero (`LightTAA::Params::calm_fresh`, `CalmTAA.step(..., fresh=True)`); `compare.py` lo segue.
`tests/quadview_math_test.cpp`, 12: la guardia a più gradini (un passo, più passi insieme, l'ultimo solo con il crollo,
il ritorno un gradino alla volta).

## [fix125] The interface's own motion

* `ui_motion_case.py make <dir> <locked|parallax|weapon> <1|2>` writes a case for `gpu/supertaa_test.exe <dir>` (the view's
  colour with interface, its copies without / with GUI, the scene's motion vectors, the block of the interface's motion
  of every frame: `d3d12::UiMotion::Constants`); mode 1 runs the analyze pass with the interface's motion, mode 2 the
  same inputs along the scene's motion vectors. `ui_motion_case.py check <dir>` compares the last frame with the picture
  the frame would have with many samples per pixel, on the writing and under a veil without detail of its own.
* `gpu/supertaa_test.exe cso <dir>` writes the compiled analyze pass, plain and with the interface's motion (the plain
  one must stay the same bytes when only the other is touched).
* `ui_steady_test.py <work dir> --speed=0 --mv_speed=3 [--ui_motion]`: `d3d12::UiSteady` with motion vectors that hold
  another motion than the interface's, without and with the block of the interface's motion.

