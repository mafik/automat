"""Generates generated/krita_<>.h for each of the listed .kra files."""
# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import sys
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path
import re
import subprocess
import fs_utils
import build_variant
import src
import make
from functools import partial
import src

kra_files = [
  fs_utils.project_root / 'source_images' / 'Mouse' / 'Mouse.kra',
  fs_utils.project_root / 'source_images' / 'Hand.kra'
]

krita_layer_to_webp = build_variant.current.BASE / 'krita_layer_to_webp'

def snake_case(name: str) -> str:
    """Convert a string to snake_case."""
    # Replace spaces and special characters with underscores
    name = re.sub(r'[^\w\s-]', '', name)
    name = re.sub(r'[-\s]+', '_', name)
    # Insert underscores before uppercase letters
    name = re.sub(r'([a-z])([A-Z])', r'\1_\2', name)
    return name.lower()


def capitalize_name(name: str) -> str:
    """Convert a snake_case name to CapitalizedWords."""
    parts = name.split('_')
    return ''.join(word.capitalize() for word in parts if word)


def parse_kra_file(kra_path: Path):
    """Parse a .kra file and extract layer information."""
    kra = zipfile.ZipFile(kra_path, 'r')

    # Read the main document XML
    maindoc_xml = kra.read('maindoc.xml').decode('utf-8')
    root = ET.fromstring(maindoc_xml)

    # Find the IMAGE element
    image = root.find('.//{http://www.calligra.org/DTD/krita}IMAGE')
    if image is None:
        raise ValueError("Could not find IMAGE element in maindoc.xml")

    width = int(image.get('width'))
    height = int(image.get('height'))
    x_res = float(image.get('x-res', '72'))
    y_res = float(image.get('y-res', '72'))

    # Find all layers
    layers_elem = image.find('.//{http://www.calligra.org/DTD/krita}layers')
    if layers_elem is None:
        raise ValueError("Could not find layers element in maindoc.xml")

    layers = []
    for layer in layers_elem.findall('.//{http://www.calligra.org/DTD/krita}layer'):
        layer_info = {
            'name': layer.get('name'),
            'visible': layer.get('visible') == '1',
            'nodetype': layer.get('nodetype'),
            'filename': layer.get('filename'),
            'x': int(layer.get('x', '0')),
            'y': int(layer.get('y', '0')),
        }
        layers.append(layer_info)

    return {
        'width': width,
        'height': height,
        'x_res': x_res,
        'y_res': y_res,
        'layers': layers,
        'kra': kra
    }


def export_paint_layer(kra_data, layer, output_path: Path):
    """Export a paint layer to a WebP file using the C++ binary.

    Returns a dict with keys: width, height, trimmed_x, trimmed_y, or None on failure.
    """
    kra = kra_data['kra']

    # The layer data is stored in Unnamed/layers/<filename>
    layer_filename = f"Unnamed/layers/{layer['filename']}"

    try:
        layer_data = kra.read(layer_filename)
    except KeyError:
        print(f"Warning: Layer data not found for {layer['name']}")
        return None

    # Run the C++ binary
    try:
        result = subprocess.run(
            [str(krita_layer_to_webp)],
            input=layer_data,
            capture_output=True,
            check=True
        )
    except subprocess.CalledProcessError as e:
        print(f"Warning: Failed to process layer {layer['name']}: {e.stderr.decode()}")
        return None
    except FileNotFoundError:
        print(f"Error: C++ binary not found at {krita_layer_to_webp}")
        print(f"Please build it first with: ./run.py 'link krita_layer_to_webp' --variant={kra_data['variant']}")
        sys.exit(1)

    # Parse stderr for metadata
    stderr_lines = result.stderr.decode().strip().split('\n')
    metadata = {}
    for line in stderr_lines:
        if line.startswith('WIDTH '):
            metadata['width'] = int(line.split()[1])
        elif line.startswith('HEIGHT '):
            metadata['height'] = int(line.split()[1])
        elif line.startswith('TRIMMED_X '):
            metadata['trimmed_x'] = int(line.split()[1])
        elif line.startswith('TRIMMED_Y '):
            metadata['trimmed_y'] = int(line.split()[1])

    # Check if we got all required metadata
    if not all(k in metadata for k in ['width', 'height', 'trimmed_x', 'trimmed_y']):
        print(f"Warning: Incomplete metadata from C++ binary for layer {layer['name']}")
        return None

    # Write WebP output
    with open(output_path, 'wb') as f:
        f.write(result.stdout)

    return metadata


def generate_transform_matrix(kra_data, layer, layer_snake: str, metadata: dict) -> str:
    """Generate C++ code for a transform matrix.

    The matrix transforms from layer texture coordinates (0,0) to (width,height)
    into metric space (meters) with origin at the center of the original canvas.
    It accounts for the layer's position and trimmed bounds.
    """
    canvas_width = kra_data['width']
    canvas_height = kra_data['height']
    x_res = kra_data['x_res']
    y_res = kra_data['y_res']

    trimmed_x = metadata['trimmed_x']
    trimmed_y = metadata['trimmed_y']
    layer_x = layer['x']
    layer_y = layer['y']

    # Scale from pixels to meters (pixels -> inches -> meters)
    # 1 inch = 0.0254 meters
    scale_x = 0.0254 / x_res
    scale_y = -0.0254 / y_res

    # Translation calculation:
    # 1. Start at texture coordinate (0, 0)
    # 2. Add layer trimmed offset (trimmed_x, trimmed_y) to get canvas pixel coordinates
    # 3. Add layer position offset (layer_x, layer_y)
    # 4. Subtract canvas center (canvas_width/2, canvas_height/2)
    # 5. Scale to meters

    trans_x = (trimmed_x + layer_x - canvas_width / 2.0) * scale_x
    trans_y = (trimmed_y + layer_y - canvas_height / 2.0) * scale_y

    return f"""const SkMatrix transform_{layer_snake} = SkMatrix::MakeAll(
    {scale_x:.10f}, 0, {trans_x:.10f},
    0, {scale_y:.10f}, {trans_y:.10f},
    0, 0, 1
);"""


def parse_svg_path_data(svg_content: str) -> tuple[str, dict, dict]:
    """Parse SVG file to extract path data, transform, and SVG info.

    Returns:
        tuple of (path_data_string, transform_dict, svg_info_dict)
        svg_info_dict contains viewBox dimensions and units
    """
    root = ET.fromstring(svg_content)

    # Extract SVG dimensions and viewBox
    svg_info = {}
    width_str = root.get('width', '0')
    height_str = root.get('height', '0')
    viewbox_str = root.get('viewBox', '0 0 0 0')

    # Parse width/height (might be in pt, px, etc.)
    # Common format: "245.76pt"
    if width_str.endswith('pt'):
        svg_info['width_pt'] = float(width_str[:-2])
        svg_info['width_unit'] = 'pt'
    elif width_str.endswith('px'):
        svg_info['width_pt'] = float(width_str[:-2])
        svg_info['width_unit'] = 'px'
    else:
        svg_info['width_pt'] = float(width_str)
        svg_info['width_unit'] = 'unknown'

    if height_str.endswith('pt'):
        svg_info['height_pt'] = float(height_str[:-2])
        svg_info['height_unit'] = 'pt'
    elif height_str.endswith('px'):
        svg_info['height_pt'] = float(height_str[:-2])
        svg_info['height_unit'] = 'px'
    else:
        svg_info['height_pt'] = float(height_str)
        svg_info['height_unit'] = 'unknown'

    # Parse viewBox
    viewbox_parts = viewbox_str.split()
    if len(viewbox_parts) == 4:
        svg_info['viewBox'] = {
            'x': float(viewbox_parts[0]),
            'y': float(viewbox_parts[1]),
            'width': float(viewbox_parts[2]),
            'height': float(viewbox_parts[3])
        }
    else:
        svg_info['viewBox'] = {'x': 0, 'y': 0, 'width': 0, 'height': 0}

    # Find the path element
    # SVG uses default namespace
    namespaces = {
        'svg': 'http://www.w3.org/2000/svg',
        'sodipodi': 'http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'
    }

    path_elem = root.find('.//svg:path', namespaces)
    if path_elem is None:
        # Try without namespace
        path_elem = root.find('.//path')

    if path_elem is None:
        raise ValueError("No path element found in SVG")

    path_data = path_elem.get('d', '')
    transform_str = path_elem.get('transform', '')

    # Parse the transform
    transform = parse_svg_transform(transform_str)

    return path_data, transform, svg_info


def parse_svg_transform(transform_str: str) -> dict:
    """Parse SVG transform string and return matrix components.

    Supports: matrix(), translate(), scale(), rotate()
    Returns dict with keys: a, b, c, d, e, f (standard SVG matrix format)
    """
    # Default to identity matrix
    result = {'a': 1.0, 'b': 0.0, 'c': 0.0, 'd': 1.0, 'e': 0.0, 'f': 0.0}

    if not transform_str:
        return result

    transform_str = transform_str.strip()

    # Parse matrix(a b c d e f) or matrix(a, b, c, d, e, f)
    if transform_str.startswith('matrix('):
        values_str = transform_str[7:-1]
        # Handle both space and comma separators
        values_str = values_str.replace(',', ' ')
        parts = values_str.split()
        if len(parts) >= 6:
            result = {
                'a': float(parts[0]),
                'b': float(parts[1]),
                'c': float(parts[2]),
                'd': float(parts[3]),
                'e': float(parts[4]),
                'f': float(parts[5])
            }

    # Parse translate(x y) or translate(x, y) or translate(x)
    elif transform_str.startswith('translate('):
        values_str = transform_str[10:-1]
        values_str = values_str.replace(',', ' ')
        parts = values_str.split()
        if len(parts) >= 1:
            result['e'] = float(parts[0])
        if len(parts) >= 2:
            result['f'] = float(parts[1])

    # Parse scale(x y) or scale(x, y) or scale(s)
    elif transform_str.startswith('scale('):
        values_str = transform_str[6:-1]
        values_str = values_str.replace(',', ' ')
        parts = values_str.split()
        if len(parts) >= 1:
            result['a'] = float(parts[0])
            result['d'] = float(parts[0])  # uniform scale by default
        if len(parts) >= 2:
            result['d'] = float(parts[1])

    # For more complex transforms (rotate, skew, or multiple transforms),
    # we'd need more sophisticated parsing. For now, warn and return identity.
    elif transform_str and not transform_str.startswith('matrix(') and not transform_str.startswith('translate(') and not transform_str.startswith('scale('):
        print(f"Warning: Unsupported SVG transform: {transform_str}")

    return result


def parse_shape_layer(kra_data, layer) -> tuple[str, dict, dict]:
    """Parse vector layer and return path data, transform, and SVG info.

    Returns:
        tuple of (svg_path_data, svg_transform_dict, svg_info_dict)
    """
    kra = kra_data['kra']

    # Shape layers are stored as .shapelayer directories with content.svg
    layer_filename = layer['filename']
    svg_path = f"Unnamed/layers/{layer_filename}.shapelayer/content.svg"

    svg_content = kra.read(svg_path).decode('utf-8')
    path_data, transform, svg_info = parse_svg_path_data(svg_content)
    return path_data, transform, svg_info


def generate_skpath_cc_impl(layer_name: str, path_data: str, svg_transform: dict, svg_info: dict, kra_data: dict, layer: dict) -> str:
    """Generate C++ code to create an SkPath from SVG path data.

    Uses Skia's SkParsePath::FromSVGString to parse the SVG path,
    then applies the appropriate transform to convert to canvas metric space.
    """
    x_res = kra_data['x_res']
    y_res = kra_data['y_res']

    layer_x = layer['x']
    layer_y = layer['y']

    # Escape the path data for C++ string literal
    # Replace backslashes and quotes
    escaped_path = path_data.replace('\\', '\\\\').replace('"', '\\"')

    # Calculate the combined transform:
    # 1. SVG local transform from the path element (in SVG point space)
    # 2. Translate to center the SVG viewBox
    # 3. Add layer position offset (convert from canvas pixels to SVG points)
    # 4. Convert from SVG points directly to meters (1 pt = 1/72 inch, 1 inch = 0.0254 m)
    # 5. Flip Y axis

    svg_a = svg_transform['a']
    svg_b = svg_transform['b']
    svg_c = svg_transform['c']
    svg_d = svg_transform['d']
    svg_e = svg_transform['e']
    svg_f = svg_transform['f']

    # Get SVG viewBox dimensions
    svg_viewbox_width = svg_info['viewBox']['width']
    svg_viewbox_height = svg_info['viewBox']['height']

    # Convert points to meters: 1 pt = 1/72 inch, 1 inch = 0.0254 m
    # So: 1 pt = 0.0254 / 72 meters
    pt_to_meter = 0.0254 / 72.0
    scale_x = pt_to_meter
    scale_y = -pt_to_meter  # Negative to flip Y axis

    # Layer position is in canvas pixels, need to convert to SVG points
    # At x_res DPI: 1 inch = x_res pixels, 1 inch = 72 points
    # Therefore: pixels_per_point = x_res / 72
    pixels_per_point_x = x_res / 72.0
    pixels_per_point_y = y_res / 72.0
    layer_x_pt = layer_x / pixels_per_point_x
    layer_y_pt = layer_y / pixels_per_point_y

    # Build combined transform matrix:
    # 1. Applies SVG local transform (from path element, in SVG point space)
    # 2. Centers the viewBox (subtract viewBox_width/2, viewBox_height/2)
    # 3. Adds layer position offset (in SVG point space)
    # 4. Converts from points to meters and flips Y

    # Calculate the centering and position offset
    translate_x = layer_x_pt - svg_viewbox_width / 2.0
    translate_y = layer_y_pt - svg_viewbox_height / 2.0

    code = f"""SkPath {layer_name}() {{
    SkPath path;
    if (!SkParsePath::FromSVGString(R"({escaped_path})", &path)) {{
        return path;
    }}
    SkMatrix transform = SkMatrix::MakeAll(
        {svg_a}, {svg_c}, {svg_e},
        {svg_b}, {svg_d}, {svg_f},
        0, 0, 1
    );
    transform.postTranslate({translate_x:.10f}, {translate_y:.10f});
    transform.postScale({scale_x}, {scale_y});
    path.transform(transform);
    return path;
}}"""

    return code


def generate_cpp_code(kra_data, file_basename: str, paint_layers_with_metadata, vector_layers_with_data) -> tuple[str, str]:
    """Generate C++ header and source files."""
    namespace = f"automat::krita::{snake_case(file_basename)}"

    # Generate header
    header_parts = [
        f"// Auto-generated from {file_basename}.kra",
        "// SPDX-FileCopyrightText: Copyright 2025 Automat Authors",
        "// SPDX-License-Identifier: MIT",
        "#pragma once",
        "",
        "#include <include/core/SkMatrix.h>",
        "#include <include/core/SkPath.h>",
        "",
        "#include \"../../src/textures.hh\"",
        "",
    ]

    # Open namespace
    header_parts.append(f"namespace {namespace} {{")
    header_parts.append("")

    # Add transform matrix declarations
    for layer, metadata in paint_layers_with_metadata:
        layer_snake = snake_case(layer['name'])
        header_parts.append(f"extern const SkMatrix transform_{layer_snake};")

    if paint_layers_with_metadata:
        header_parts.append("")

    # Add PersistentImage declarations
    for layer, metadata in paint_layers_with_metadata:
        layer_snake = snake_case(layer['name'])
        header_parts.append(f"extern PersistentImage {layer_snake};")

    if paint_layers_with_metadata:
        header_parts.append("")

    # Add vector layer function declarations
    for layer, path_data, svg_transform, svg_info in vector_layers_with_data:
        layer_name = capitalize_name(snake_case(layer['name']))
        header_parts.append(f"SkPath {layer_name}();")

    # Close namespace
    header_parts.append("")
    header_parts.append(f"}}  // namespace {namespace}")
    header_parts.append("")

    # Generate source
    source_parts = [
        f"// Auto-generated from {file_basename}.kra",
        "// SPDX-FileCopyrightText: Copyright 2025 Automat Authors",
        "// SPDX-License-Identifier: MIT",
        "",
        f'#include "krita_{snake_case(file_basename)}.hh"',
        "",
        '#include "embedded.hh"',
        "",
        '#include <include/utils/SkParsePath.h>',
        "",
    ]

    # Open namespace
    source_parts.append(f"namespace {namespace} {{")
    source_parts.append("")

    # Add transform matrix definitions
    for layer, metadata in paint_layers_with_metadata:
        layer_snake = snake_case(layer['name'])
        source_parts.append(generate_transform_matrix(kra_data, layer, layer_snake, metadata))
        source_parts.append("")

    # Add PersistentImage definitions
    for layer, metadata in paint_layers_with_metadata:
        layer_snake = snake_case(layer['name'])
        file_snake = snake_case(file_basename)
        embedded_name = f"embedded::assets_krita_{file_snake}_{layer_snake}_webp"
        source_parts.append(f"PersistentImage {layer_snake} = PersistentImage::MakeFromAsset({embedded_name}, {{.matrix = transform_{layer_snake}}});")
        source_parts.append("")

    # Add vector layer function definitions
    for layer, path_data, svg_transform, svg_info in vector_layers_with_data:
        layer_name = capitalize_name(snake_case(layer['name']))
        source_parts.append(generate_skpath_cc_impl(layer_name, path_data, svg_transform, svg_info, kra_data, layer))
        source_parts.append("")

    # Close namespace
    source_parts.append(f"}}  // namespace {namespace}")
    source_parts.append("")

    return '\n'.join(header_parts), '\n'.join(source_parts)

def extract_kra_layers(kra_path):
    file_basename = kra_path.stem
    # Parse the .kra file
    kra_data = parse_kra_file(kra_path)

    # Separate visible paint and vector layers
    paint_layers = []
    vector_layers = []

    for layer in kra_data['layers']:
        if not layer['visible']:
            continue

        if layer['nodetype'] == 'paintlayer':
            paint_layers.append(layer)
        elif layer['nodetype'] in ['vectorlayer', 'shapelayer']:
            vector_layers.append(layer)

    # print(f"Found {len(paint_layers)} visible paint layers and {len(vector_layers)} visible vector layers")

    # Export paint layers to WebP
    assets_dir = Path('assets')
    assets_dir.mkdir(exist_ok=True)

    exported_paint_layers_with_metadata = []
    for layer in paint_layers:
        layer_snake = snake_case(layer['name'])
        output_path = assets_dir / f"krita-{file_basename.lower()}-{layer_snake}.webp"
        metadata = export_paint_layer(kra_data, layer, output_path)
        if metadata is not None:
            exported_paint_layers_with_metadata.append((layer, metadata))

    # Export vector layers (parse SVG data)
    exported_vector_layers_with_data = []
    for layer in vector_layers:
        try:
            path_data, svg_transform, svg_info = parse_shape_layer(kra_data, layer)
            exported_vector_layers_with_data.append((layer, path_data, svg_transform, svg_info))
        except Exception as e:
            print(f"Warning: Failed to export vector layer {layer['name']}: {e}")

    # Generate C++ code
    if exported_paint_layers_with_metadata or exported_vector_layers_with_data:
        output_dir = Path('build/generated')
        output_dir.mkdir(parents=True, exist_ok=True)

        header_code, source_code = generate_cpp_code(
            kra_data, file_basename, exported_paint_layers_with_metadata, exported_vector_layers_with_data
        )

        header_path = output_dir / f"krita_{snake_case(file_basename)}.hh"
        source_path = output_dir / f"krita_{snake_case(file_basename)}.cc"

        header_path.write_text(header_code)
        source_path.write_text(source_code)

    kra_data['kra'].close()

# Force the "embedded" extension to be loaded before this one.
embedded = src.load_extension('embedded')

def hook_srcs(srcs: dict[str, src.File], recipe: make.Recipe):
    for kra_path in kra_files:
        if not kra_path.exists():
            continue

        file_snake = snake_case(kra_path.stem)
        hh_path = fs_utils.generated_dir / f'krita_{file_snake}.hh'
        cc_path = fs_utils.generated_dir / f'krita_{file_snake}.cc'

        recipe.generated.add(str(hh_path))
        recipe.generated.add(str(cc_path))

        hh_file = src.File(hh_path)
        hh_file.direct_includes.append(str(fs_utils.src_dir / 'textures.hh'))
        srcs[str(hh_path)] = hh_file
        cc_file = src.File(cc_path)
        cc_file.direct_includes.append(str(fs_utils.generated_dir / 'embedded.hh'))
        srcs[str(cc_path)] = cc_file

        recipe.add_step(partial(extract_kra_layers, kra_path), [hh_path, cc_path],
                        [kra_path, krita_layer_to_webp, Path(__file__)],
                        desc=f'Exporting {kra_path.name} layers',
                        shortcut=f'krita {file_snake}')

        embedded.main_step.inputs.add(str(hh_path))
        embedded.main_step.inputs.add(str(cc_path))
