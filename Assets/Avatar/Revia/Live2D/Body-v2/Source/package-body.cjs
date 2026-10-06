'use strict';

// Run from any directory: node package-body.cjs.
// Install sharp and ag-psd beside this script, or set REVIA_SHARP_MODULE and
// REVIA_AG_PSD_MODULE to their installed package directories. Only Body-v2 is written.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const assert = require('node:assert/strict');
const sharp = require(process.env.REVIA_SHARP_MODULE || 'sharp');
const { initializeCanvas, writePsdBuffer, readPsd } = require(process.env.REVIA_AG_PSD_MODULE || 'ag-psd');

initializeCanvas(() => { throw new Error('Canvas conversion is not used.'); },
    (width, height) => ({ width, height, data: new Uint8ClampedArray(width * height * 4) }));

const outputRoot = path.resolve(__dirname, '..');
const existingRoot = path.resolve(outputRoot, '..');
const referencePath = path.join(__dirname, 'new-body-reference.png');
const canvas = { width: 1024, height: 1536 };
const names = ['Arm_ViewerLeft', 'Arm_ViewerRight', 'Body'];
const transform = {
    scale: 0.685,
    sourceAnchor: [522, 300],
    targetAnchor: [512, 432],
    placement: [154, 227],
    affineOutputOffset: [0.43, -0.5],
    interpolation: 'bicubic'
};
// Source-coordinate boundaries follow the collar and jacket neckline. Generated
// head/hair above this line is excluded; no replacement pixels are painted.
const neckline = [
    [0, 360], [325, 338], [350, 317], [395, 304], [427, 308], [456, 325],
    [482, 317], [490, 293], [555, 293], [560, 312], [596, 318], [624, 317],
    [650, 305], [677, 319], [704, 346], [1024, 360]
];
// [x,y] seam coordinates. Every admitted pixel belongs to exactly one part, so
// there is no overlap or missing seam pixel at the neutral pose.
const leftSeam = [[424, 330], [442, 416], [430, 490], [418, 570], [400, 645], [376, 721], [354, 780], [326, 810], [330, 910]];
const rightSeam = [[620, 330], [598, 405], [609, 490], [621, 570], [640, 650], [665, 728], [684, 786], [710, 817], [700, 910]];

const hash = data => crypto.createHash('sha256').update(data).digest('hex');
const hashFile = filename => hash(fs.readFileSync(filename));

function interpolate(points, value, inputAxis, outputAxis)
{
    if (value <= points[0][inputAxis]) return points[0][outputAxis];
    for (let index = 1; index < points.length; ++index)
    {
        if (value > points[index][inputAxis]) continue;
        const first = points[index - 1], second = points[index];
        const amount = (value - first[inputAxis]) / (second[inputAxis] - first[inputAxis]);
        return first[outputAxis] + amount * (second[outputAxis] - first[outputAxis]);
    }
    return points.at(-1)[outputAxis];
}

function bounds(raw, width, height)
{
    let left = width, top = height, right = -1, bottom = -1;
    for (let y = 0; y < height; ++y)
    {
        for (let x = 0; x < width; ++x)
        {
            if (!raw[(y * width + x) * 4 + 3]) continue;
            left = Math.min(left, x); top = Math.min(top, y);
            right = Math.max(right, x); bottom = Math.max(bottom, y);
        }
    }
    assert(right >= 0, 'A packaged part is empty.');
    return { left, top, width: right - left + 1, height: bottom - top + 1 };
}

function pasteDisjoint(target, pixels, placement)
{
    const [left, top, width, height] = placement;
    for (let y = 0; y < height; ++y)
    {
        for (let x = 0; x < width; ++x)
        {
            const from = (y * width + x) * 4;
            if (!pixels[from + 3]) continue;
            const to = ((y + top) * canvas.width + x + left) * 4;
            assert.equal(target[to + 3], 0, 'Part ownership overlaps at a seam.');
            pixels.copy(target, to, from, from + 4);
        }
    }
}

async function main()
{
    fs.mkdirSync(path.join(outputRoot, 'Parts'), { recursive: true });
    const input = path.resolve(process.argv[2] || referencePath);
    const inputBytes = fs.readFileSync(input);
    if (fs.existsSync(referencePath))
        assert.equal(hashFile(referencePath), hash(inputBytes), 'Use a new sibling version for a different body reference.');
    else
        fs.writeFileSync(referencePath, inputBytes);
    const oldLayoutPath = path.join(existingRoot, 'Source/layout.json');
    const oldPsdPath = path.join(existingRoot, 'revia-parts.psd');
    const oldLayout = JSON.parse(fs.readFileSync(oldLayoutPath, 'utf8'));
    const oldPsdHash = hashFile(oldPsdPath);
    const oldLayoutHash = hashFile(oldLayoutPath);
    const oldPsd = readPsd(fs.readFileSync(oldPsdPath), { useImageData: true });
    assert.equal(oldPsd.width, canvas.width); assert.equal(oldPsd.height, canvas.height);
    const headLayers = [];
    for (const item of oldLayout.layers.filter(layer => !names.includes(layer.name)))
    {
        const filename = path.join(existingRoot, 'Parts', `${item.name}.png`);
        const decoded = await sharp(filename).ensureAlpha().raw().toBuffer({ resolveWithObject: true });
        assert.equal(decoded.info.width, item.placement[2]);
        assert.equal(decoded.info.height, item.placement[3]);
        const psdLayer = oldPsd.children.find(layer => layer.name === item.name);
        assert(psdLayer && psdLayer.imageData, `Missing original PSD head layer: ${item.name}`);
        assert.equal(psdLayer.left, item.placement[0]); assert.equal(psdLayer.top, item.placement[1]);
        assert(decoded.data.equals(Buffer.from(psdLayer.imageData.data)), `Original PNG/PSD head mismatch: ${item.name}`);
        headLayers.push({ ...item, filename, pngSha256: hashFile(filename), rgbaSha256: hash(decoded.data) });
    }
    const original = await sharp(referencePath).ensureAlpha().raw().toBuffer({ resolveWithObject: true });
    assert.equal(original.info.width, 1024); assert.equal(original.info.height, 1536);
    const masked = Buffer.from(original.data);
    let excludedOpaquePixels = 0;
    for (let y = 0; y < original.info.height; ++y)
    {
        for (let x = 0; x < original.info.width; ++x)
        {
            const index = (y * original.info.width + x) * 4;
            const excluded = y + 0.5 < interpolate(neckline, x + 0.5, 0, 1);
            if (excluded && masked[index + 3]) ++excludedOpaquePixels;
            if (excluded || !masked[index + 3]) masked.fill(0, index, index + 4);
        }
    }
    const scaled = await sharp(masked, { raw: original.info })
        .affine([transform.scale, 0, 0, transform.scale], {
            background: { r: 0, g: 0, b: 0, alpha: 0 },
            interpolator: sharp.interpolators.bicubic,
            odx: transform.affineOutputOffset[0], ody: transform.affineOutputOffset[1]
        }).raw().toBuffer({ resolveWithObject: true });
    const expected = Buffer.alloc(canvas.width * canvas.height * 4);
    const owned = Object.fromEntries(names.map(name => [name, Buffer.alloc(expected.length)]));
    for (let y = 0; y < scaled.info.height; ++y)
    {
        for (let x = 0; x < scaled.info.width; ++x)
        {
            const from = (y * scaled.info.width + x) * 4;
            if (!scaled.data[from + 3]) continue;
            const px = x + transform.placement[0], py = y + transform.placement[1];
            assert(px >= 0 && py >= 0 && px < canvas.width && py < canvas.height, 'Body exceeds the output canvas.');
            const to = (py * canvas.width + px) * 4;
            const sx = (x + 0.5 - transform.affineOutputOffset[0]) / transform.scale;
            const sy = (y + 0.5 - transform.affineOutputOffset[1]) / transform.scale;
            let name = 'Body';
            if (sy <= 910 && sx < interpolate(leftSeam, sy, 1, 0)) name = 'Arm_ViewerLeft';
            else if (sy <= 910 && sx >= interpolate(rightSeam, sy, 1, 0)) name = 'Arm_ViewerRight';
            scaled.data.copy(expected, to, from, from + 4);
            scaled.data.copy(owned[name], to, from, from + 4);
        }
    }
    const layers = [], parts = [];
    for (const name of names)
    {
        const rectangle = bounds(owned[name], canvas.width, canvas.height);
        const cropped = await sharp(owned[name], { raw: { ...canvas, channels: 4 } }).extract(rectangle)
            .raw().toBuffer({ resolveWithObject: true });
        const png = await sharp(cropped.data, { raw: cropped.info }).png().toBuffer();
        const filename = path.join(outputRoot, 'Parts', `${name}.png`);
        fs.writeFileSync(filename, png);
        const placement = [rectangle.left, rectangle.top, rectangle.width, rectangle.height];
        parts.push({ name, filename, placement, pngSha256: hash(png), rgbaSha256: hash(cropped.data), pixels: cropped.data });
        layers.push({ name, left: rectangle.left, top: rectangle.top, hidden: false, opacity: 1, blendMode: 'normal',
            imageData: { width: rectangle.width, height: rectangle.height, data: new Uint8ClampedArray(cropped.data) } });
    }
    const bodyPreview = await sharp(expected, { raw: { ...canvas, channels: 4 } }).png().toBuffer();
    fs.writeFileSync(path.join(outputRoot, 'body-only-preview.png'), bodyPreview);
    const psdPath = path.join(outputRoot, 'body-update.psd');
    fs.writeFileSync(psdPath, writePsdBuffer({ ...canvas, children: layers,
        imageData: { ...canvas, data: new Uint8ClampedArray(expected) } }, { generateThumbnail: false, noBackground: true }));
    const compositing = [];
    for (const item of oldLayout.layers)
    {
        if (item.hidden) continue;
        const replacement = parts.find(part => part.name === item.name);
        const placement = replacement ? replacement.placement : item.placement;
        const filename = replacement ? replacement.filename : path.join(existingRoot, 'Parts', `${item.name}.png`);
        compositing.push({ input: filename, left: placement[0], top: placement[1] });
    }
    const full = sharp({ create: { ...canvas, channels: 4, background: { r: 0, g: 0, b: 0, alpha: 0 } } }).composite(compositing);
    await full.clone().png().toFile(path.join(outputRoot, 'fullbody-preview.png'));
    await full.clone().flatten({ background: '#d9dbe0' }).png().toFile(path.join(outputRoot, 'fullbody-preview-gray.png'));
    const readback = readPsd(fs.readFileSync(psdPath), { useImageData: true });
    assert.equal(readback.width, canvas.width); assert.equal(readback.height, canvas.height);
    assert.equal(readback.children.length, 3);
    assert.deepEqual(readback.children.map(layer => layer.name), names);
    const reconstructed = Buffer.alloc(expected.length);
    for (const layer of readback.children)
    {
        const part = parts.find(item => item.name === layer.name);
        const pixels = Buffer.from(layer.imageData.data);
        assert(pixels.equals(part.pixels), `PSD readback pixels changed: ${layer.name}`);
        assert.equal(layer.left, part.placement[0]); assert.equal(layer.top, part.placement[1]);
        pasteDisjoint(reconstructed, pixels, part.placement);
    }
    assert(reconstructed.equals(expected), 'PSD parts do not reconstruct the transformed masked body exactly.');
    for (const item of headLayers) assert.equal(hashFile(item.filename), item.pngSha256, `Original head PNG changed: ${item.name}`);
    assert.equal(hashFile(oldPsdPath), oldPsdHash); assert.equal(hashFile(oldLayoutPath), oldLayoutHash);
    const manifest = {
        version: 2, canvas, layerOrder: 'back-to-front',
        input: { file: 'new-body-reference.png', sha256: hash(inputBytes), originalRgbaSha256: hash(original.data) },
        transform, masks: { coordinates: 'original 1024x1536 source pixel centers', neckline, leftSeam, rightSeam,
            partitionAfterScaling: true, armEndSourceY: 910, excludedOpaquePixels,
            transparentRgbPolicy: 'Original copied verbatim; zero-alpha RGB canonicalized only in derived previews/parts.' },
        parts: parts.map(({ pixels, filename, ...item }) => ({ ...item, file: `../Parts/${path.basename(filename)}` })),
        unchangedHead: { originalLayout: '../../Source/layout.json', originalLayoutSha256: oldLayoutHash,
            originalPsd: '../../revia-parts.psd', originalPsdSha256: oldPsdHash,
            layers: headLayers.map(({ filename, ...item }) => ({ ...item, file: `../../Parts/${path.basename(filename)}` })) },
        verification: { psdLayerCount: 3, pngPsdHeadPixelsIdentical: true, unchangedHeadPngCount: headLayers.length,
            headPngHashesUnchanged: true, pixelReconstructionIdentical: true, reconstructedRgbaSha256: hash(reconstructed),
            derivedBodyBounds: bounds(expected, canvas.width, canvas.height),
            nominalHeadCrownY: 245, nominalHeadChinY: 426,
            nominalTransformedFeetY: transform.placement[1] + 1475 * transform.scale + transform.affineOutputOffset[1],
            nominalHeightInHeads: ((transform.placement[1] + 1475 * transform.scale + transform.affineOutputOffset[1]) - 245) / 181 },
        dependencies: { sharp: sharp.versions.sharp, vips: sharp.versions.vips, agPsd: '31.0.2' },
        limitation: 'Geometric separation preserves source pixels without hidden underpaint. Seams and proportions require native visual QA before rig deformation.'
    };
    fs.writeFileSync(path.join(__dirname, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n');
    console.log(JSON.stringify({ psd: psdPath, previews: ['body-only-preview.png', 'fullbody-preview.png', 'fullbody-preview-gray.png'],
        parts: manifest.parts, verification: manifest.verification }, null, 2));
}

main().catch(error => { console.error(error.message); process.exitCode = 1; });
