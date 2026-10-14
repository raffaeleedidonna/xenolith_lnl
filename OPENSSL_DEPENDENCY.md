# Perché Xenolith usa OpenSSL 3

Xenolith usa **OpenSSL 3 come dipendenza opzionale a runtime su Linux** per
verificare rapidamente l’integrità dei pesi usati dalla vision. La libreria
richiesta è `libcrypto.so.3`; non servono gli header di sviluppo per compilare
Xenolith.

La modifica è nel commit `c82fedc`, `Accelerate vision weight verification
with optional OpenSSL 3`.

## Il problema che abbiamo risolto

Il supporto vision usa il modello Gemma 4 e un `mmproj` compatibile, che contiene
i pesi dell’encoder visuale e del proiettore. Abbiamo fissato versioni e
checksum di entrambi nel [manifesto](vision/manifest.json).

All’avvio del percorso visuale, la verifica completa calcola il SHA-256 dei
file e lo confronta con i valori attesi. Questo permette di rilevare anche
modifiche ai valori dei tensori che lasciano invariati i metadati GGUF.

Il modello occupa circa 14,25 GB e il `mmproj` circa 1,19 GB. Inizialmente
usavamo un’implementazione SHA-256 in C generico: il calcolo su questi file
aggiungeva decine di secondi all’avvio. Il controllo completo era stato
introdotto con la vision; il percorso precedente usava controlli rapidi su
header e metadati.

## Perché OpenSSL

OpenSSL offre implementazioni crittografiche ottimizzate e seleziona percorsi
adatti alle capacità della CPU, comprese le estensioni SHA disponibili sulla
Lunar Lake. È già presente sulla macchina di test.

**Con OpenSSL leggiamo e verifichiamo ancora l’intero contenuto dei file.**
L’algoritmo e il checksum atteso restano gli stessi; cambia l’implementazione
del calcolo. Non usiamo OpenSSL per modificare i pesi, elaborare immagini o
generare risposte. Questa integrazione non richiede connessioni di rete.

Il codice carica opzionalmente `libcrypto.so.3` tramite `dlopen`, cerca
`EVP_Q_digest` e richiede il digest `SHA256`. Il risultato viene confrontato
con il manifesto. Il confronto tra l’implementazione OpenSSL e quella C serve
nei test e non viene ripetuto durante il normale avvio.

## Risultati sulla Lunar Lake

La prova con la stessa foto del gattino ha dato questi risultati:

| Fase | Prima | Con OpenSSL |
|---|---:|---:|
| Apertura modello e verifica | 35,587 s | 4,021 s |
| Caricamento/verifica mmproj e inizializzazione | 3,775 s | 0,741 s |
| Prova completa con due domande | 67,651 s | 31,408 s |

Sono campioni singoli, senza controllo dello stato della cache disco e senza
stima della variabilità. La prova completa comprende caricamento, encoder,
prefill e due risposte: non è una misura del tempo al primo token. I confronti
isolati del calcolo SHA-256 hanno mostrato un’accelerazione di circa dieci
volte. L’encoder visuale resta un costo distinto e non è stato ottimizzato da
questa modifica.

## Cosa succede quando OpenSSL non è disponibile

Se la libreria non si carica, manca la funzione richiesta oppure il calcolo
restituisce un errore, Xenolith **mostra un avviso e usa i controlli rapidi** su
dimensione, header, metadati e struttura dei tensori. Le impronte degli header
attesi sono anch’esse fissate nel manifesto.

Non ripiega sul lento SHA-256 in C dell’intero file. Questa scelta conserva
un avvio rapido anche senza OpenSSL. Il controllo rapido verifica struttura
e compatibilità, ma può lasciare passare modifiche ai soli valori dei tensori.

Un SHA-256 completo calcolato correttamente ma diverso da quello atteso fa
invece **rifiutare i pesi**: non attiva il fallback.

Le cache degli embedding e del contesto distinguono le modalità di verifica;
nel fallback includono anche l’identità del file e i timestamp. Il cambio di
modalità conserva i media delle sessioni e ricostruisce le cache incompatibili.

Per provare il fallback senza modificare l’installazione:

```sh
XENOLITH_WEIGHTS_VERIFY=metadata ./xenolith chat MODEL.gguf \
  --ctx 65536 --mmproj mmproj-F16.gguf --image gattino.jpg
```

La variabile vale solo per quel comando e produce un avviso esplicito.

## Impatto sulle dipendenze e verifiche

Per un’installazione standard Linux conviene includere la libreria OpenSSL 3,
così la verifica completa è disponibile. Xenolith resta avviabile quando
questa dipendenza opzionale manca; non richiede un collegamento obbligatorio
a `libcrypto` durante la compilazione.

I test sulla Lunar Lake hanno verificato digest, errori simulati della libreria,
fallback e passaggi tra modalità nelle sessioni persistenti. I quattro dump
testuali a 64k/128k coincidono con la baseline precedente alla vision e gli
embedding visuali verificati coincidono byte per byte con i riferimenti già
validati.

Misure e limiti sono in [VALIDATION.md](vision/VALIDATION.md#verifica-pesi-accelerata-e-fallback-rapido),
con i campioni nel [report di startup](vision/results/openssl-startup.json).
Il caricamento e il fallback sono implementati in [format.c](format.c).
Per le API e la selezione delle capacità CPU si vedano le documentazioni
ufficiali [EVP digest](https://docs.openssl.org/3.0/man3/EVP_DigestInit/) e
[OPENSSL_ia32cap](https://docs.openssl.org/3.0/man3/OPENSSL_ia32cap/).
