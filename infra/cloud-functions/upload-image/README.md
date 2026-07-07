# Cloud Function: upload-image

> Note: This is an optional legacy draft stub for HTTP image ingestion.
> In the active production architecture (CAM v2.2.0), the ESP32-CAM executes leaf-health analysis on-device in C++ (`VisionBotanist.cpp`) and communicates directly with Firebase Storage and Firestore via C++ SDKs (`CloudService.cpp`), bypassing this Cloud Function and MQTT entirely.

## Purpose

HTTP endpoint that receives a `multipart/form-data` upload from the ESP32-CAM and:

1. Validates `device_id` and `image` (JPEG file).
2. Uploads the blob to **Firebase Storage** under `smartvase/{device_id}/images/{ts}.jpg`.
3. Returns JSON `{ "image_url": "<signed_url>", "ok": true }`.

The CAM can optionally use this endpoint to store blobs. In the primary production workflow, however, the ESP32-CAM uploads directly to Google Firebase Storage and Firestore without relying on MQTT publishing or secondary Python cloud pipelines.

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

## Security Considerations and TODO

- Auth via App Check / Identity Token instead of `--allow-unauthenticated`.
- Pin the CA cert in the CAM (currently `client.setInsecure()`).
- Rate limiting per device_id.
- JPEG magic-byte validation (FF D8 FF).
