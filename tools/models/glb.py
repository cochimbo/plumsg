"""A small glTF 2.0 binary (.glb) writer for the models plumsg makes itself.

Each primitive is a triangle list with positions, normals, and optionally texture
coordinates and vertex colours, drawn with one material (a base colour, optionally a
PNG or JPEG texture). Positions are in metres, y up, the front towards +z.
"""

import io
import json
import struct

import numpy as np


class Glb:
    def __init__(self):
        self.bin = bytearray()
        self.views, self.accessors, self.meshes_prims = [], [], []
        self.materials, self.textures, self.images = [], [], []

    def _view(self, data: bytes, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        view = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            view["target"] = target
        self.bin += data
        self.views.append(view)
        return len(self.views) - 1

    def _accessor(self, array, kind, component, target, minmax=False):
        array = np.ascontiguousarray(array)
        view = self._view(array.tobytes(), target)
        acc = {"bufferView": view, "componentType": component, "count": int(array.shape[0]), "type": kind}
        if minmax:
            acc["min"] = array.min(axis=0).tolist()
            acc["max"] = array.max(axis=0).tolist()
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def image(self, pil_image, jpeg=False):
        buf = io.BytesIO()
        if jpeg:
            pil_image.convert("RGB").save(buf, "JPEG", quality=90)
        else:
            pil_image.save(buf, "PNG", optimize=True)
        view = self._view(buf.getvalue())
        self.images.append({"bufferView": view, "mimeType": "image/jpeg" if jpeg else "image/png"})
        self.textures.append({"source": len(self.images) - 1, "sampler": 0})
        return len(self.textures) - 1

    def material(self, name, colour, texture=None, roughness=0.5):
        pbr = {"baseColorFactor": [colour[0], colour[1], colour[2], 1.0], "metallicFactor": 0.0, "roughnessFactor": roughness}
        if texture is not None:
            pbr["baseColorTexture"] = {"index": texture}
        self.materials.append({"name": name, "pbrMetallicRoughness": pbr})
        return len(self.materials) - 1

    def add(self, vertices, faces, normals, material, uvs=None, colours=None):
        attrs = {
            "POSITION": self._accessor(np.asarray(vertices, np.float32), "VEC3", 5126, 34962, True),
            "NORMAL": self._accessor(np.asarray(normals, np.float32), "VEC3", 5126, 34962),
        }
        if uvs is not None:
            attrs["TEXCOORD_0"] = self._accessor(np.asarray(uvs, np.float32), "VEC2", 5126, 34962)
        if colours is not None:
            c = np.asarray(colours, np.float32)
            if c.shape[1] == 3:
                c = np.hstack([c, np.ones((len(c), 1), np.float32)])
            attrs["COLOR_0"] = self._accessor(c, "VEC4", 5126, 34962)
        idx = np.asarray(faces, np.uint32).reshape(-1)
        indices = self._accessor(idx.reshape(-1, 1), "SCALAR", 5125, 34963)
        self.accessors[indices]["type"] = "SCALAR"
        self.meshes_prims.append({"attributes": attrs, "indices": indices, "material": material})

    def save(self, path, credit):
        doc = {
            "asset": {"version": "2.0", "generator": "plumsg tools/models", "copyright": credit},
            "scene": 0,
            "scenes": [{"nodes": [0]}],
            "nodes": [{"mesh": 0}],
            "meshes": [{"primitives": self.meshes_prims}],
            "materials": self.materials,
            "accessors": self.accessors,
            "bufferViews": self.views,
            "buffers": [{"byteLength": len(self.bin)}],
        }
        if self.images:
            doc["images"] = self.images
            doc["textures"] = self.textures
            doc["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071}]
        js = json.dumps(doc, separators=(",", ":")).encode()
        js += b" " * ((4 - len(js) % 4) % 4)
        while len(self.bin) % 4:
            self.bin.append(0)
        total = 12 + 8 + len(js) + 8 + len(self.bin)
        with open(path, "wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, total))
            f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
            f.write(struct.pack("<II", len(self.bin), 0x004E4942) + bytes(self.bin))
