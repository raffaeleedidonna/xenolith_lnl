# Vision: verifiche e misure sulla Lunar Lake

14 ottobre 2026. Implementazione sperimentale nativa C/Level Zero per il
checkpoint Gemma 4 26B-A4B IT QAT bloccato in [manifest.json](manifest.json).
Sono verificati encoder, risposta CLI e sessioni/protocollo a 64k e 128k.
I risultati descrivono fixture determinate; non certificano la qualità generale
su fotografie, documenti o screenshot reali. L'output rimane testuale.

## Ambiente e provenienza

Core Ultra 7 268V, iGPU Lunar Lake 0x64a0, WSL Ubuntu 24.04, kernel
6.18.54.1, GCC 13.3.0; Intel compute runtime 26.31.39395.14 e loader Level Zero
1.32.0. Dettagli in [environment.json](results/environment.json).
Memoria WSL circa 27,41 GiB. Tutte le prove hanno usato copie isolate;
installazione e servizi originali non sono stati sostituiti.

Baseline Xenolith: `36a4c12cdf75bbfb05426233dae54037ba1d6641`.
Modello e mmproj sono stati verificati sulla macchina target con SHA-256
completo, dimensione e revisione della distribuzione. Le versioni di
Transformers, llama.cpp, NumPy e Pillow, insieme agli hash dei riferimenti,
sono nel manifesto. I numeri README dell'HP i7-13700H appartengono a un
altro dispositivo e non costituiscono una baseline per la Lunar Lake.

## Numerica dell'encoder

Soglie fissate prima del confronto: RMS relativo <= 0,005 e coseno >= 0,99998.
Non sono state aumentate. Il confronto completo con l'oracle CPU allineato
controlla 30 stadi: embedding iniziale con posizioni, tutti i 27 layer,
standardizzazione e proiezione finale. Tutti passano a 70, 280 e 1120 righe.

| Righe | RMS embedding finale | Coseno | RMS massimo nei 30 stadi |
|---:|---:|---:|---:|
| 70 | 0,001742 | 0,999998483 | 0,001834 |
| 280 | 0,002910 | 0,999995765 | 0,003059 |
| 1120 | 0,004073 | 0,999991706 | 0,004073 |

I report [70](results/encoder70-f32gelu-comparison.json),
[280](results/encoder280-f32gelu-comparison.json) e
[1120](results/encoder1120-f32gelu-comparison.json) conservano anche gli hash
SHA-256 dei dump e gli errori per stadio.

Il riferimento llama.cpp richiede due adattamenti espliciti, non modifiche
ai pesi. Il GGUF omette `clip.use_gelu`, mentre il checkpoint prescrive
GELU-tanh: `vision_reference.cpp` seleziona `FFN_GELU`. Inoltre ggml CPU
applica una tabella GELU F16 anche a intermedi F32. Il confronto con quella
tabella diverge fino a superare la soglia a 280; il confronto allineato F32
usa una copia isolata delle sorgenti, modificata da
[vision_reference_patch.py](../tools/vision_reference_patch.py), con hash
prima/dopo nel manifesto. La libreria del decoder linguistico conserva la
configurazione CPU originale. I risultati iniziali, inclusi quelli della
variante LUT, restano distinti dai risultati allineati.

NumPy implementa indipendentemente le equazioni del riferimento Transformers,
con input delle dense arrotondati a F16 e accumuli F32. Il confronto dell'encoder
corrente sui quattro stadi selezionati a 70/280 e su ultimo layer/proiezione a
1120 passa con le stesse soglie. RMS finale: 0,001736 / 0,002866 / 0,004355.
Report: [70](results/encoder70-v2-numpy.json),
[280](results/encoder280-numpy.json), [1120](results/encoder1120-numpy.json).
Un primo confronto NumPy completo di tutti i 30 stadi a 70, prima della revisione
striped32, è conservato separatamente in [encoder70-numpy.json](results/encoder70-numpy.json).

Il preprocessing ha superato 25 casi: 15 geometrie/budget PNG, otto
orientamenti EXIF, PNG con alpha su bianco e JPEG orientato. PNG, resize e
alpha coincidono byte per byte con Pillow e la formula HF bloccata. Nel JPEG
la differenza massima di canale è 3, entro la tolleranza fissata per i decoder
JPEG diversi. Un file troncato viene rifiutato. [Report](results/preprocessing.json).

## Attenzione, risposte e sessioni

Il test GPU confronta la maschera visuale con il calcolo esplicito CPU:
17 righe con 7 elementi precedenti e 1120 righe con 1023 precedenti. Errore
massimo rispettivamente circa 3,18e-7 e 1,98e-6. Verifica inoltre il commit
circolare a 512/1024/1120 righe e offset che attraversano il wraparound.
Non aggiunge padding al contesto. I layer locali vedono le righe successive
della stessa immagine entro la semantica della finestra; i globali restano causali.

La CLI ha riconosciuto il quadrante rosso a tutti i budget: 70, 140 (126 righe),
280, 560 (532 righe), 1120. La revisione corrente ha ripetuto la prova a 1120
con contesto 128k. Il confronto iniziale del decoder CPU llama.cpp a 70, sugli
stessi embedding visuali e token del prompt, ha RMS logits 0,013302 e coseno
0,9999356, stesso argmax e insieme top-10, con due posizioni scambiate.
Entrambi producono `Red` seguito dalla fine turno. È una misura diagnostica
della differenza tra motori, distinta dalle soglie dell'encoder e dalle
regressioni contro la baseline Xenolith. [Report iniziale](results/cli-report.json).

Il confronto completo del decoder a 1120 righe/128k produce RMS logits
0,013916 e coseno 0,9999183, stesso argmax e insieme top-10; entrambi generano
`red` e fine turno. [Report](results/decoder1120-comparison.json).
Il riferimento mantiene FA disabilitata, sei thread di decode e otto di
prefill; `n_batch=n_ubatch=1120`, con `swa_full=false`. La configurazione
predefinita `swa_full=true` aveva tentato di allocare 25.600 MiB di KV nei
soli layer locali a 128k ed era stata interrotta con segnale 9. La cache
limitata conserva finestra più batch (2304 celle arrotondate), senza spezzare
il blocco visuale. Il confronto è poi terminato correttamente, senza swap.

I test C sul modello reale verificano snapshot, identità, modifica degli
embedding a parità di ID, COW/shadow, promozione, rewind e proprietà della
memoria. La versione finale aggiunge una seconda immagine conservando il KV
precedente e confronta i logits con un replay completo: identici byte per byte.
Le fixture usano 70 righe a 64k e 1120 righe a 128k.

Il client minimo [vision_wire_check.py](../tools/vision_wire_check.py) verifica:

- capacità v2 e rifiuto di immagini/input invalidi senza modificare la storia;
- cronologia, SHA dei media e recupero dei byte originali;
- risposta, checkpoint, immagine diversa con uguale numero di righe, rewind;
- riavvio del processo e caricamento dello snapshot visuale;
- cancellazione della cache embeddings e ricostruzione dai media persistenti;
- ephemeral senza alterare la cronologia, rebuild da ID con budget diverso;
- immagine in un risultato di tool;
- compatibilità v1 e rifiuto esplicito delle sessioni visuali senza mmproj.

Entrambi i contesti verificano anche rollback del rebuild, cancellazione
durante reasoning, replay dopo la proiezione della cronologia e OCR del codice
sintetico `7318`. Tutti i 15 controlli passano dopo il fix dei dispatch: [64k](results/wire64.json), [128k](results/wire128.json).
I contesti sono capacità configurate; queste prove non riempiono 64k o 128k
conversazioni di immagini. Il costo della cache e la qualità a contesto pieno
richiedono una campagna distinta.

Una prova di riavvio ha rilevato che l'UUID del driver Intel incorpora un
timestamp di inizializzazione, confermato nel
[codice Intel](https://github.com/intel/compute-runtime/blob/master/level_zero/core/source/driver/driver_handle.cpp).
L'identità numerica ora lega versione driver, UUID dispositivo e modulo GPU
nativo compilato, oltre a sorgenti, compiler/flags e SPIR-V. Il riavvio riusa
cache e snapshot, mentre i cambi numerici reali invalidano il riuso.

## Prestazioni e memoria

Il default testuale rimane 512. La matrice iniziale con tre misure per cella
usa gli stessi benchmark esistenti `pp512`, `pp2048`, `pp8192`, `tg128`,
contesti 64k/128k e stesso host. I campioni sono in
[benchmark-matrix.json](results/benchmark-matrix.json), etichettati come fase
CLI iniziale. A 1024, i prompt lunghi risultano circa 20–25% più lenti di 512;
il confronto numerico sui prompt da 2048/8192 è identico nei casi provati.
Il benchmark conclusivo, dopo il fix dei dispatch, confronta baseline/candidate
in ordine ABBA, tre campioni per esecuzione e due esecuzioni per variante/contesto,
con warmup e pausa di un secondo. Mediane dei sei campioni per variante, in token/s:

| Contesto | Test | Baseline | Candidate | Variazione |
|---:|---|---:|---:|---:|
| 64k | pp2048 | 224,20 | 227,21 | +1,34% |
| 64k | tg128 | 23,54 | 23,49 | -0,20% |
| 128k | pp2048 | 225,25 | 224,46 | -0,35% |
| 128k | tg128 | 23,36 | 23,80 | +1,86% |

Non emerge una regressione del default 512 nei casi misurati. Gli intervalli
dei campioni si sovrappongono; queste differenze inferiori al 2% non dimostrano
un miglioramento sistematico. [Campioni e riepilogo](results/benchmark-paired-final.json),
[hash dei sorgenti/eseguibili](results/final-provenance.json).
La precedente campagna ABBA, con dieci campioni per variante, è conservata
[separatamente](results/benchmark-paired-final-before-dispatch-fix.json) e
precede la correzione del percorso senza tracing.

I quattro dump testuali contro la baseline, dopo 512/2048 elementi nei due
contesti, sono identici byte per byte: [report](results/text-regression-final.json).
Anche i logits finali del testo reale da 2048/8192 token coincidono a 512 e
1024, in entrambi i contesti: [report](results/text-batch-final.json).
Le suite esistenti `check-unit`, `check-context`, `check-gpu`, `test_profile`,
`test_runtime`, `test_runtime_model`, `test_serve` e `test_serve_model` sono
passate. `check-vision` e il test input con ASan/UBSan su Linux sono passati.
Dopo il fix dei dispatch sono stati ripetuti `check-unit`, `check-context`,
`check-gpu`, `check-vision`, le due sessioni wire complete, la sessione
1120 con riavvio e i dump testuali contro la baseline.
[Log e fasi delle verifiche](results/checks-final.json).

Misure encoder della revisione striped32 prima del fix dei dispatch,
con dump diagnostici abilitati:

| Righe | Preprocessing | Encoder | Hash modello completo | Caricamento mmproj |
|---:|---:|---:|---:|---:|
| 70 | 49,7 ms | 2,515 s | 35,445 s | 4,679 s |
| 280 | 60,0 ms | 17,181 s | 31,079 s | 3,059 s |
| 1120 | 81,4 ms | 199,717 s | 36,362 s | 3,728 s |

A 1120/128k, prefill LLM 8,638 s, durata totale CLI 260,17 s, RSS massimo
17.904.776 KiB (17,08 GiB), `time -v` riporta zero swap. La CLI libera
allocazioni dell'encoder prima di caricare il modello linguistico. La lettura
degli hash avviene una volta per engine; nelle sessioni pesa sullo startup,
mentre gli embedding verificati vengono riusati.

La CLI prima del fix dei dispatch, senza dump al budget predefinito 280,
risponde `Red`:
encoder 18,567 s, prefill LLM 2,903 s. L'encoder CPU allineato F32, otto
thread e senza dump, impiega 7,766 / 23,982 / 161,735 s a 70 / 280 / 1120.
La proiezione finale senza callback diagnostici passa gli stessi confronti
numerici. Sono misure singole, separate dallo startup e prive di stime di
variabilità; a 1120 la misura GPU con dump non è direttamente omogenea alla
misura CPU senza dump. [Campioni](results/extra-measurements.json).
La ripetizione finale di `pp2048 --prefill 1024` dà 169,84 e 168,57 token/s
a 64k/128k, circa il 24% sotto le mediane a 512 misurate nella stessa fase. Il default resta 512.

Dopo il fix dei dispatch, i percorsi senza tracing danno queste misure singole,
separate dallo startup e senza una stima della variabilità:

| Righe | Encoder nativo GPU | Oracle CPU F32 |
|---:|---:|---:|
| 280 (default) | 18,612 s | 23,982 s |
| 1120 | 213,229 s | 161,735 s |

Al budget predefinito gli embedding coincidono byte per byte con quelli
già verificati nei 30 stadi. Preprocessing 73,751 ms, hash completo del modello
39,342 s, caricamento mmproj 3,981 s. [Report](results/encoder280-final-only.json).
A 1120 l'encoder nativo è circa il 32% più lento dell'oracle CPU in queste prove. L'output nativo è identico byte per byte al precedente
output tracciato. L'ottimizzazione del budget massimo rimane lavoro necessario
per una buona latenza interattiva; non è nascosta dal confronto testuale.
Il runtime persistente 128k produce `Red`, salva automaticamente il checkpoint
e conclude senza tracing a 1120 righe. Dopo il riavvio, la stessa sessione
carica lo snapshot di 1162 elementi (`resume.loaded=true`) e risponde ancora
`Red` alla domanda successiva. RSS massimo osservato nel run di riuso
26.858.560 KiB, `VmSwap=0`. [Sessione e riavvio](results/wire128-budget1120.json),
[encoder senza tracing](results/encoder1120-final-only.json).

Nelle sessioni a 128k, con modello e mmproj contemporaneamente disponibili,
il massimo RSS storico osservato del processo di test è 26.977.408 KiB
(25,73 GiB). Le otto letture tardive di `/proc` riportano `VmSwap=0` e circa
9 GiB di `MemAvailable`: [misure](results/wire128-memory.json). RSS storico,
memoria fisica disponibile e residenza GPU sono misure diverse, particolarmente
con allocazioni condivise; questi campioni non costituiscono una traccia
continua della memoria fisica di picco. Il workspace di prefill, separatamente,
è 212,33 / 416,67 / 454,98 MiB a 512 / 1024 / 1120 e non include l'encoder.

La latenza a 1120 è ancora elevata. Questa versione fornisce funzionalità e
riferimenti verificati; non promette interattività a budget massimo. Il runtime
visuale usa replay sincrono e disabilita lo shadow prefill asincrono per prompt
con immagini. La preparazione visuale precede gli eventi di generazione.

La prova finale senza tracing ha rilevato una perdita del dispositivo GPU a
1120: le attese introdotte dai dump avevano nascosto il problema. Il percorso
normale ora sincronizza i dispatch e suddivide le query dell'attenzione visuale
in gruppi di 128 patch, mantenendo tutte le chiavi/valori disponibili in ogni
gruppo. `vision-encode --final-only` verifica il percorso senza callback o dump
intermedi: a 1120 gli embedding coincidono byte per byte con i dump precedenti
già validati contro CPU/NumPy. Le regressioni testuali, le sessioni e i
benchmark conclusivi sono ripetuti dopo questa correzione; le misure precedenti
restano identificate separatamente.

## Chat interattiva con un’immagine

Il comando `chat --mmproj FILE --image FILE` collega l’immagine alla prima
richiesta e mantiene il prefisso visuale per le domande successive. Accetta un
solo allegato iniziale; `/exit` termina la chat. Per nuovi allegati nei turni
successivi e persistenza si usa il protocollo v2. Il percorso testuale senza
immagine conserva il comportamento precedente.

Questo adattamento dell’interfaccia CLI è successivo alla campagna ABBA sopra
riportata: i suoi hash in `final-provenance.json` identificano l’eseguibile dei
benchmark. Encoder, kernel e dimensione del blocco testuale restano quelli
misurati. Le verifiche specifiche della chat e gli hash del nuovo eseguibile
sono raccolti nel [report](results/chat-kitten.json). Due turni sul JPEG reale
del gattino e due turni testuali passano; i 143 controlli dei parametri passano.
Il log conferma una sola esecuzione dell’encoder per la conversazione visuale.

## Verifica pesi accelerata e fallback rapido (storico)

Questa sezione descrive la campagna del precedente loader OpenSSL. Il loader
corrente riusa lo SHA completo di `main`, senza OpenSSL né fallback ai soli
metadati. I risultati sottostanti restano evidenza storica, non descrivono la
policy corrente; vedere [INPUT_FORMAT.md](INPUT_FORMAT.md).

L’aggiornamento successivo al commit `a7f7e68` usa l’API `EVP_Q_digest` della
libreria OpenSSL 3 già disponibile su Linux, caricata opzionalmente senza
richiedere header di sviluppo. Il risultato viene confrontato con gli SHA-256
completi fissati nel manifesto; il confronto OpenSSL/C appartiene ai test e
non viene ripetuto durante l’avvio dell’applicazione.

Come richiesto dall’utente, se la libreria manca o il calcolo fallisce il loader
avvisa e usa header, metadati, directory tensori e dimensione del file. Non
ripiega sulla scansione C completa dei tensori. Le impronte rapide sono quelle
dello schema MODEL v2 precedente alla vision e sono ora bloccate nel manifesto:
header del modello 15.824.000 byte, header mmproj 22.816 byte. Questa modalità
controlla compatibilità e struttura; modifiche ai soli valori dei tensori con
metadati invariati possono passare. `XENOLITH_WEIGHTS_VERIFY=metadata` consente
la verifica esplicita di questo fallback, sempre con avviso.

La prova sulla stessa foto del gattino riporta questi tempi singoli:

| Fase | Prima | OpenSSL | Solo metadati |
|---|---:|---:|---:|
| Apertura modello e verifica | 35,587 s | 4,021 s | 0,141 s |
| mmproj, verifica e inizializzazione | 3,775 s | 0,741 s | 0,522 s |
| Encoder | 17,706 s | 17,282 s | 19,182 s |
| Prova completa con due domande | 67,651 s | 31,408 s | 41,219 s |

Il tempo totale comprende anche caricamento/repack del modello linguistico,
prefill e due risposte; non misura il solo tempo al primo prompt. Le prove
sono singole, senza stima della variabilità o controllo dello stato iniziale
della cache disco. Il fallback evita il costo della scansione C; il suo tempo
totale non è risultato minore della prova OpenSSL. I costi rimanenti di
caricamento e calcolo richiedono misure separate.

Sono passati i test dei digest esatti, bordi di blocco, input disallineati e
inizializzazione concorrente, inclusi simbolo EVP assente, funzione fallita,
lunghezza digest errata e ASan/UBSan. La selezione del fallback su un buffer
inaccessibile di lunghezza massima dimostra che non viene letto come file
completo. Un test su pagina COW privata rileva una modifica ai tensori con
SHA completo e verifica che i soli metadati la accettino; i metadati corrotti
sono rifiutati in entrambe le modalità. Gli artefatti originali restano intatti.

Le suite host/context/GPU/vision passano. Gli embedding a 70 sono identici
byte per byte al riferimento numerico precedente in entrambe le modalità;
i quattro dump testuali 64k/128k restano identici alla baseline pre-vision.
Le due chat riconoscono il gatto e descrivono il mantello.

Il fingerprint visuale v3 lega la modalità di verifica e, nel fallback, anche
inode/device, dimensione e mtime/ctime. Il test su una sessione persistente
full → metadata → metadata → full mantiene l’immagine e la risposta. I due
cambi di modalità ricostruiscono il contesto; il riavvio nella stessa modalità
metadata carica lo snapshot. I media persistenti rimangono utilizzabili;
embedding e KV incompatibili non vengono riusati. Campioni, log e checksum
sono nel [report](results/openssl-startup.json).

## Integrazione dei checkpoint upstream

Il branch integra il `main` di Simone al commit
`4521185b381bf9f48e5a12ff9abb05eb5a3a164b`, inclusa la deduplicazione e
conservazione per budget introdotta da `fbd03cf`. I kernel e il calcolo
dell’encoder restano quelli della versione vision precedente.

Gli snapshot testuali usano il formato upstream v2 e l’ABI numerica stabile.
Gli snapshot visuali usano v3, con hash dell’input misto a byte 320 e ABI a
byte 352. Questo evita la collisione tra il precedente v2 visuale e il v2
testuale upstream. I vecchi snapshot visuali sono riconosciuti e rifiutati
conservativamente: il runtime ricostruisce il contesto dai media originali.
Il bridge v1 testuale richiede il kernel storico; questa build vision lo ha
modificato e ricostruisce il prefisso anziché dichiararlo compatibile.

I checkpoint di una conversazione condividono i segmenti della KV globale
tramite l’envelope `XEKVSEG` e file `.xkc`. Il controllo dei predecessori
include identità delle immagini, embedding, media, budget e confini completi;
le immagini successive al checkpoint sono escluse dal suo prefisso. La
potatura segue il riferimento durevole nel transcript. Il ripristino cerca
anche checkpoint precedenti quando quello più recente manca o viene rifiutato.

`--cache-budget` limita checkpoint e segmenti condivisi, mentre gli embedding
hanno un limite separato di 2 GiB. Con budget zero entrambi i percorsi di cache
su disco sono disabilitati; i file esistenti e i media persistenti restano.

Il nuovo test host `tests/test_visual_cache.c` usa il serializer/cache reale
con KV deterministica: verifica v3/ABI, riconoscimento del vecchio v2 visuale,
immagini diverse con stessi token/righe, variazioni di embedding/media/budget,
prefissi prima di una seconda immagine, rifiuto dei confini interni, riserve,
deduplicazione, riapertura, cache disabilitata e rimozione dei segmenti.
Per checkpoint visuali a 256/512 elementi lo spazio passa da 175.116.544 byte
standalone complessivi a 169.876.512 byte condivisi. È una fixture di storage,
non una misura di qualità visuale o della memoria GPU.

Il test upstream a 32k/64k su buffer host passa con ripristino esatto:
2.435.187.968 byte standalone contro 1.764.406.976 condivisi, risparmiando
670.780.992 byte entro un budget di 2 GiB. Non implica lo stesso risparmio
per ogni conversazione.

Le suite host/context/GPU/vision e persistenza passano sulla Lunar Lake.
Il confronto tra processi conferma il riuso testuale v2 senza prefill; il
vecchio v1 con kernel storico viene rifiutato e ricostruito con logits e
continuazione identici a 64 e 1057 elementi, inclusa la finestra SWA avvolta.

Le prove end-to-end a 64k e 128k passano: migrazione da un eseguibile
pre-integrazione reale, cronologia/media invariati, ricostruzione v2 visuale,
riuso v3 dopo riavvio, recupero da un predecessore quando l’ultimo checkpoint
è corrotto, budget zero senza modificare le cache e rimozione per budget senza
perdere l’immagine. Passano anche tutti i controlli del protocollo visuale,
inclusi seconda immagine con diversa risposta, rewind, tool result, rebuild,
ephemeral, cancellazione e OCR sintetico. Il cambio OpenSSL → metadati →
metadati → OpenSSL conserva i media e rispetta la compatibilità delle cache.

I quattro dump testuali a 64k/128k coincidono byte per byte con la baseline
pre-vision. L’encoder a 70 produce lo stesso SHA-256 già validato
(`7872f946040c3872034f57d54a15cfda858df35ddbc0fc07ba1423b41fa56587`).
La chat del gattino riconosce il gatto e descrive lo stesso mantello.
La sessione 128k/1120 senza tracing passa risposta, checkpoint e riavvio con
riuso dello snapshot; la lettura finale di memoria riporta `VmSwap=0` e
`VmHWM=26.119.396 KiB`. Non è una traccia continua della memoria GPU.

Confronto ABBA sulla Lunar Lake rispetto all’eseguibile vision/OpenSSL
precedente all’integrazione: 3 ripetizioni per esecuzione, 6 campioni per
variante e contesto, warmup, prefill testuale 512 e pausa di 1 secondo.

| Contesto | Fase | Mediana precedente (token/s) | Mediana integrata (token/s) | Differenza |
|---|---|---:|---:|---:|
| 64k | pp2048 | 225,79 | 235,19 | +4,16% |
| 64k | tg128 | 23,15 | 22,81 | −1,50% |
| 128k | pp2048 | 231,86 | 228,24 | −1,56% |
| 128k | tg128 | 23,82 | 24,71 | +3,77% |

Gli intervalli min/max dei campioni si sovrappongono in tutti i confronti:
non emerge una regressione rilevante da queste misure. Le variazioni positive
non dimostrano un’accelerazione del calcolo, che non è stato modificato.
Campioni, hash dei sorgenti/eseguibili, esiti e checksum dei log sono nel
[report dell’integrazione](results/upstream-cache-integration.json).

Per riprodurre la migrazione con due eseguibili e directory isolate:

```sh
make check-unit check-context check-gpu check-vision
make check-cache-large
make check-persistence check-runtime check-serve MODEL=/path/to/model.gguf
make check-snapshot-compat-model MODEL=/path/to/model.gguf
python3 tools/vision_cache_integration_check.py \
  --engine ./xenolith --previous-engine /path/to/pre-integration/xenolith \
  --model /path/to/model.gguf --mmproj /path/to/mmproj-F16.gguf \
  --image vision/fixtures/quadrants.png --ctx 65536 --output vision-runs/migration64
```

Ripetere l’ultimo comando con `--ctx 131072` e un output nuovo. Il controllo
dei formati storici richiede il commit baseline nella storia Git oppure
`LEGACY_SOURCE` con l’export corrispondente; compila il kernel storico separato
quando quello corrente è cambiato, senza allargare il bridge di compatibilità.

## Riproduzione

Sul target Linux/Level Zero, con il modello già disponibile:

```sh
MODEL=/path/to/gemma-4-26B-A4B-it-qat-UD-Q4_K_XL.gguf
mkdir -p vision-runs
python3 tools/vision_artifacts.py --download --references --model "$MODEL" \
  --inventory vision-runs/mmproj-inventory.json
make -j4 xenolith check-unit check-context check-gpu check-vision
make tools/vision_prepare
python3 tools/vision_fixtures.py
python3 tools/vision_preprocess_check.py
mkdir -p vision-runs/gpu70 vision-runs/numpy70
./xenolith vision-encode "$MODEL" --mmproj .vision-cache/mmproj-F16.gguf \
  --image vision/fixtures/quadrants.png --image-tokens 70 \
  --vision-dump vision-runs/gpu70
python3 tools/vision_numpy_reference.py .vision-cache/mmproj-F16.gguf \
  vision/fixtures/quadrants-70.rgb vision-runs/numpy70
python3 tools/vision_compare.py vision-runs/numpy70 vision-runs/gpu70 \
  --output vision-runs/encoder70.json
python3 tools/vision_wire_check.py --engine ./xenolith --model "$MODEL" \
  --mmproj .vision-cache/mmproj-F16.gguf --fixtures vision/fixtures \
  --ctx 65536 --output vision-runs/session64
python3 tools/vision_large_session_check.py --engine ./xenolith --model "$MODEL" \
  --mmproj .vision-cache/mmproj-F16.gguf --fixtures vision/fixtures \
  --output vision-runs/session128-large
```

Creare la directory `vision-runs` prima di esportare l'inventario. Usare
NumPy 2.0.2 e Pillow 11.3.0 per i riferimenti. Ripetere il test wire con
`--ctx 131072` e una directory output nuova. La versione corrente del client
di test comprende anche cancellazione e OCR per entrambi i contesti.
Per ottenere la baseline, compilare un checkout isolato del commit indicato;
[vision_bench.py](../tools/vision_bench.py) esegue la matrice esistente e
conserva campioni, comandi e hash degli eseguibili. Non eseguire inferenze o
compilazioni concorrenti durante le misure.

Per il confronto ABBA conclusivo:

```sh
python3 tools/vision_bench.py --model "$MODEL" --baseline /path/to/baseline/xenolith \
  --candidate ./xenolith --paired --repetitions 3 --output vision-runs/paired
```

## Integrazione locale dell'identità completa da main

L'integrazione di `88e8661` sul branch vision `28c544e` riusa lo SHA di
`format.c`, elimina il loader OpenSSL e distingue compatibilità strutturale,
identità dei contenuti e artefatti di riferimento. Il proiettore ora controlla
i metadati numerici richiesti e tutti i 356 tensori, inclusi tipi, dimensioni,
limiti e sovrapposizioni. Gli hash del manifest restano riferimenti diagnostici.

Le verifiche mirate sull'i7-13700H comprendono:

- Loader reale su tre GGUF sintetici compatibili con contenuti/header diversi,
  SHA completo confrontato con `hashlib`, e rifiuto di 26 strutture non valide;
  gli stessi casi passano con UBSan.
- Replay dei media persistiti, cambio di modello/proiettore, isolamento e riuso
  della cache, e rifiuto di media o identità derivate incoerenti. Il test host
  passa anche con ASan/UBSan (rilevamento dei leak disabilitato).
- Modello e proiettore reali del manifest: modifica dei contenuti rilevata dal
  digest, variante strutturalmente compatibile accettata, header errato rifiutato.
- Test GPU vision; per un PNG rosso e budget 70, i 64 embedding risultanti
  (720.896 byte F32) sono identici byte per byte tra la PR originale e questa
  integrazione. È una verifica di regressione, non una nuova campagna di qualità.
- Checkpoint testuali v2 fra build correnti e migrazione dei writer storici v1/v2
  con identità dei soli metadati: rifiuto senza alterare lo stato, ricostruzione
  a 64/1057 token e continuazione esatta su decode CPU ed estensione GPU.
- Servizio reale a contesto 65536: avvio vision con cache zero, riapertura di una
  conversazione della PR originale, cambio del proiettore e successivo riavvio.
  I media restano disponibili, la vecchia KV non viene caricata, e il riavvio
  con gli stessi pesi carica il checkpoint a 144 token. Tutte le risposte
  identificano il rosso; ogni avvio corrente calcola il digest del modello una
  sola volta, prima delle richieste.

La policy corrente e l'eccezione di startup per `--mmproj` sono documentate in
[PROTOCOL.md](../PROTOCOL.md) e [INPUT_FORMAT.md](INPUT_FORMAT.md). I report
storici OpenSSL e i precedenti risultati numerici restano riferiti ai rispettivi
commit, senza attribuirli al codice modificato.
