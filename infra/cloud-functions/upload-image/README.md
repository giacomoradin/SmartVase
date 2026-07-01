# Cloud Function — upload-image

> ⚠️ Stub: this is a draft to be refined with **Fia** (Cloud Architect).
> Consistent with the 2026-05-19 decision (HiveMQ + Cloud Functions + Firestore).

## Purpose

HTTP endpoint that receives a `multipart/form-data` upload from the ESP32-CAM and:

1. Validates `device_id` and `image` (JPEG file).
2. Uploads the blob to **Firebase Storage** under `smartvase/{device_id}/images/{ts}.jpg`.
3. Returns JSON `{ "image_url": "<signed_url>", "ok": true }`.

The CAM then autonomously publishes to MQTT `smartvase/{device_id}/vision/image`
with the `image_url`. A **second** Cloud Function (out of scope here) reads
that topic from HiveMQ, downloads the image, invokes the Python pipeline in
`vision/` and publishes the result to `smartvase/{device_id}/vision/result`.

## API contract

**Request** (`POST` with `Content-Type: multipart/form-data`):

| Field       | Type   | Notes                                |
|-------------|--------|-----------------------------------------|
| `device_id` | string | E.g. `CAM_A1B2C3`                    |
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

**Errors**:
- `400` invalid payload / missing device_id.
- `413` image > size limit.
- `500` upload failed.

## Implementation

See `index.js` (Node.js 20 + `@google-cloud/storage` + `busboy`).

## Deploy (placeholder)

```
gcloud functions deploy upload-image \
  --gen2 --region=europe-west1 \
  --runtime=nodejs20 --entry-point=uploadImage \
  --trigger-http --allow-unauthenticated \
  --set-env-vars=BUCKET=<your-bucket-name>,MAX_BYTES=200000
```

The resulting URL must be saved in the CAM's NVS under the `upload_url` key
(see the comment in its `main.cpp`).

## Security (Fia TODO)

- Auth via App Check / Identity Token instead of `--allow-unauthenticated`.
- Pin the CA cert in the CAM (currently `client.setInsecure()`).
- Rate limiting per device_id.
- JPEG magic-byte validation (FF D8 FF).
