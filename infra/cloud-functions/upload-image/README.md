# Cloud Function — upload-image

> ⚠️ Stub: questa è una bozza da rifinire con **Fia** (Cloud Architect).
> Coerente con la decisione del 2026-05-19 (HiveMQ + Cloud Functions + Firestore).

## Scopo

Endpoint HTTP che riceve un upload `multipart/form-data` dalla ESP32-CAM e:

1. Valida `device_id` e `image` (file JPEG).
2. Carica il blob su **Firebase Storage** sotto `smartvase/{device_id}/images/{ts}.jpg`.
3. Restituisce JSON `{ "image_url": "<signed_url>", "ok": true }`.

La CAM poi pubblica autonomamente su MQTT `smartvase/{device_id}/vision/image`
con la `image_url`. Una **seconda** Cloud Function (out-of-scope qui) legge
quel topic da HiveMQ, scarica l'immagine, invoca la pipeline Python in
`vision/` e pubblica il risultato su `smartvase/{device_id}/vision/result`.

## Contratto API

**Request** (`POST` con `Content-Type: multipart/form-data`):

| Campo       | Tipo   | Note                                |
|-------------|--------|-------------------------------------|
| `device_id` | string | Es. `CAM_A1B2C3`                    |
| `image`     | file   | JPEG, max 200 KB                    |

**Response 200**:

```json
{
  "ok": true,
  "image_url": "https://firebasestorage.googleapis.com/.../capture.jpg?token=...",
  "size_bytes": 24576,
  "stored_at": "smartvase/CAM_A1B2C3/images/1716123456.jpg"
}
```

**Errori**:
- `400` payload non valido / device_id mancante.
- `413` image > size limit.
- `500` upload fallito.

## Implementazione

Vedi `index.js` (Node.js 20 + `@google-cloud/storage` + `busboy`).

## Deploy (placeholder)

```
gcloud functions deploy upload-image \
  --gen2 --region=europe-west1 \
  --runtime=nodejs20 --entry-point=uploadImage \
  --trigger-http --allow-unauthenticated \
  --set-env-vars=BUCKET=<your-bucket-name>,MAX_BYTES=200000
```

URL risultante va salvato in NVS della CAM sotto la chiave `upload_url`
(vedi commento nel suo `main.cpp`).

## Sicurezza (TODO Fia)

- Auth via App Check / Identity Token invece di `--allow-unauthenticated`.
- Pin del CA cert nella CAM (oggi `client.setInsecure()`).
- Rate limiting per device_id.
- Validazione magic bytes JPEG (FF D8 FF).
