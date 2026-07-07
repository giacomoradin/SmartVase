/**
 * SmartVase — Cloud Function `upload-image` (STUB)
 *
 * HTTP gen2 endpoint. Receives multipart/form-data from the ESP32-CAM,
 * uploads the JPEG to Firebase Storage and returns a signed URL.
 *
 * NOTE: this is a draft to be refined with Fia. Possible refinements:
 *   - Auth via Identity Token / App Check.
 *   - device_id validation (regex on CAM_XXXXXX).
 *   - JPEG magic-bytes validation.
 *   - Rate limiting (Cloud Armor or middleware).
 */

const { Storage } = require('@google-cloud/storage');
const Busboy      = require('busboy');

const BUCKET    = process.env.BUCKET    || 'smartvase-images';
const MAX_BYTES = parseInt(process.env.MAX_BYTES || '200000', 10);
// Signed URL TTL (s) — 24h di default.
const URL_TTL_SEC = parseInt(process.env.URL_TTL_SEC || '86400', 10);

const storage = new Storage();

function parseMultipart(req) {
  return new Promise((resolve, reject) => {
    if (req.method !== 'POST') {
      return reject(Object.assign(new Error('method_not_allowed'), { code: 405 }));
    }
    const ct = req.headers['content-type'] || '';
    if (!ct.startsWith('multipart/form-data')) {
      return reject(Object.assign(new Error('bad_content_type'), { code: 400 }));
    }

    const bb = Busboy({ headers: req.headers, limits: { fileSize: MAX_BYTES } });
    const fields = {};
    let fileBuf = null;
    let fileTooLarge = false;
    let fileMimeType = 'application/octet-stream';

    bb.on('field', (name, val) => { fields[name] = val; });

    bb.on('file', (_name, file, info) => {
      fileMimeType = info.mimeType || fileMimeType;
      const chunks = [];
      file.on('data', (c) => chunks.push(c));
      file.on('limit', () => { fileTooLarge = true; });
      file.on('end',   () => { fileBuf = Buffer.concat(chunks); });
    });

    bb.on('error', reject);
    bb.on('finish', () => {
      if (fileTooLarge) return reject(Object.assign(new Error('image_too_large'), { code: 413 }));
      if (!fileBuf)     return reject(Object.assign(new Error('image_missing'),  { code: 400 }));
      if (!fields.device_id) return reject(Object.assign(new Error('device_id_missing'), { code: 400 }));

      // Validazione magic bytes JPEG (FF D8 FF).
      if (fileBuf.length < 3 ||
          fileBuf[0] !== 0xFF || fileBuf[1] !== 0xD8 || fileBuf[2] !== 0xFF) {
        return reject(Object.assign(new Error('not_a_jpeg'), { code: 400 }));
      }

      resolve({ device_id: fields.device_id, buffer: fileBuf, mimeType: fileMimeType });
    });

    if (req.rawBody) bb.end(req.rawBody);
    else             req.pipe(bb);
  });
}

exports.uploadImage = async (req, res) => {
  try {
    const { device_id, buffer, mimeType } = await parseMultipart(req);
    const ts = Math.floor(Date.now() / 1000);
    const objectPath = `smartvase/${device_id}/images/${ts}.jpg`;

    const bucket = storage.bucket(BUCKET);
    const file = bucket.file(objectPath);
    await file.save(buffer, {
      contentType: mimeType,
      resumable: false,
      metadata: { cacheControl: 'public, max-age=3600' },
    });

    const [signedUrl] = await file.getSignedUrl({
      action: 'read',
      expires: Date.now() + URL_TTL_SEC * 1000,
    });

    res.status(200).json({
      ok: true,
      image_url: signedUrl,
      size_bytes: buffer.length,
      stored_at: objectPath,
    });
  } catch (err) {
    const code = err.code && Number.isInteger(err.code) ? err.code : 500;
    res.status(code).json({ ok: false, error: err.message || String(err) });
  }
};
