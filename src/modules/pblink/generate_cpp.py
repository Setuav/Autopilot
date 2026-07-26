#!/usr/bin/env python3
"""
generate_cpp.py - PX4 Build Generator Script

Executed during CMake build time (`make px4_sitl_pblink`).
Reads topics.yaml and pre-existing .proto files, renders Jinja2 templates,
and outputs C++ converters and header files.
"""
import os
import argparse
import re
import yaml
from jinja2 import Environment, FileSystemLoader

def uorb_to_proto_type(uorb_type):
    type_map = {
        'uint64': 'uint64',
        'uint32': 'uint32',
        'uint16': 'uint32',
        'uint8': 'uint32',
        'int64': 'int64',
        'int32': 'int32',
        'int16': 'int32',
        'int8': 'int32',
        'float': 'float',
        'double': 'double',
        'bool': 'bool',
        'float32': 'float',
        'float64': 'double',
    }
    return type_map.get(uorb_type, "bytes")

def snake_to_camel(snake_case_string):
    return "".join(word.capitalize() for word in snake_case_string.split('_'))

def parse_msg_file(msg_path):
    fields = []
    field_regex = re.compile(r'^\s*([a-zA-Z0-9_]+)(?:\[(\d+)\])?\s+([a-zA-Z0-9_]+).*')

    with open(msg_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#') or '=' in line:
                continue

            match = field_regex.match(line)
            if match:
                uorb_type, array_size, name = match.groups()
                supported_types = ['uint64', 'uint32', 'uint16', 'uint8', 'int64', 'int32', 'int16', 'int8', 'float', 'double', 'bool', 'float32', 'float64', 'char']
                fields.append({
                    'name': name,
                    'type': uorb_type,
                    'is_array': array_size is not None,
                    'array_size': int(array_size) if array_size else 0,
                    'is_supported': uorb_type in supported_types
                })
    return fields

def find_msg_file(msg_dir, topic_name_pascal):
    if not msg_dir or not os.path.exists(msg_dir):
        return None
    candidate = os.path.join(msg_dir, f"{topic_name_pascal}.msg")
    if os.path.exists(candidate):
        return candidate
    return None

def write_generated(filepath, content):
    os.makedirs(os.path.dirname(filepath), exist_ok=True)
    if os.path.exists(filepath):
        with open(filepath, 'r') as f:
            if f.read() == content:
                return
    with open(filepath, 'w') as f:
        f.write(content)

def process_topic(topic, msg_dir, proto_template, package_name, proto_dir, yaml_file_path):
    topic_name_snake = topic['name']
    topic_name_camel = snake_to_camel(topic_name_snake)

    custom_proto_path = os.path.join(os.path.dirname(yaml_file_path), "custom", f"{topic_name_snake}.proto")
    if not os.path.exists(custom_proto_path):
        custom_proto_path = os.path.join(os.path.dirname(yaml_file_path), "generated", f"{topic_name_snake}.proto")

    if os.path.exists(custom_proto_path):
        import shutil
        proto_output_path = os.path.join(proto_dir, f"{topic_name_snake}.proto")
        shutil.copyfile(custom_proto_path, proto_output_path)
        custom_options_path = custom_proto_path.replace(".proto", ".options")
        if os.path.exists(custom_options_path):
            options_output_path = os.path.join(proto_dir, f"{topic_name_snake}.options")
            shutil.copyfile(custom_options_path, options_output_path)

        rate_hz = topic.get('rate_hz', 0)
        interval_us = int(1e6 / rate_hz) if rate_hz > 0 else 0
        return {
            'name': topic_name_snake,
            'name_pascal': topic_name_camel,
            'name_upper': topic_name_snake.upper(),
            'fields': [],
            'msg_type_id': topic.get('msg_type_id', 0),
            'rate_hz': rate_hz,
            'interval_us': interval_us,
            'description': topic.get('description', ''),
            'internal': topic.get('internal', False)
        }

    if 'msg_file' in topic:
        explicit_filename = topic['msg_file']
        possible_paths = [
            os.path.join(os.path.dirname(yaml_file_path), explicit_filename),
            os.path.join(os.path.dirname(yaml_file_path), "custom", explicit_filename),
            os.path.join(msg_dir, explicit_filename),
            os.path.join(msg_dir, "versioned", explicit_filename)
        ]

        msg_file_path = None
        for path in possible_paths:
            if os.path.exists(path):
                msg_file_path = path
                break

        if not msg_file_path:
            return None
    else:
        msg_file_path = find_msg_file(msg_dir, topic_name_camel)
        if not msg_file_path:
            return None

    fields = parse_msg_file(msg_file_path)

    rate_hz = topic.get('rate_hz', 0)
    interval_us = int(1e6 / rate_hz) if rate_hz > 0 else 0

    topic_data = {
        'name': topic_name_snake,
        'name_pascal': topic_name_camel,
        'name_upper': topic_name_snake.upper(),
        'fields': fields,
        'msg_type_id': topic.get('msg_type_id', 0),
        'rate_hz': rate_hz,
        'interval_us': interval_us,
        'description': topic.get('description', ''),
        'internal': topic.get('internal', False)
    }

    proto_output_path = os.path.join(proto_dir, f"{topic_name_snake}.proto")
    proto_content = proto_template.render(
        package_name=package_name,
        message_name=topic_name_camel,
        fields=fields
    )
    write_generated(proto_output_path, proto_content)

    return topic_data

def main():
    parser = argparse.ArgumentParser(description="PX4 Build Tool: Generate C++ converter files from Protobuf and uORB schemas.")
    parser.add_argument('-y', '--yaml-file', required=True, help="YAML config file")
    parser.add_argument('-m', '--msg-dir', help="PX4 msg directory")
    parser.add_argument('-t', '--template-dir', help="Jinja2 templates directory")
    parser.add_argument('-p', '--proto-dir', help="Output directory for .proto files")
    parser.add_argument('-g', '--generated-dir', help="Output directory for C++ converter files")

    args = parser.parse_args()

    with open(args.yaml_file, 'r') as f:
        config = yaml.safe_load(f)

    package_name = config.get('package', 'px4_pblink.msgs')
    topics = config.get('topics', [])

    env = Environment(loader=FileSystemLoader(args.template_dir))
    env.filters['uorb_to_proto_type'] = uorb_to_proto_type
    proto_template = env.get_template('topic.proto.j2')

    uplink_topics = []
    downlink_topics = []
    unique_topics = []
    seen_ids = set()

    for topic in topics:
        topic_data = process_topic(topic, args.msg_dir, proto_template, package_name, args.proto_dir, args.yaml_file)
        if topic_data:
            direction = topic.get('direction', 'uplink')
            if direction == 'uplink':
                uplink_topics.append(topic_data)
            elif direction == 'downlink':
                downlink_topics.append(topic_data)
            elif direction == 'both':
                topic_data_uplink = topic_data.copy()
                topic_data_uplink['rate_hz'] = topic.get('uplink_rate_hz', 10)
                topic_data_uplink['interval_us'] = int(1e6 / topic_data_uplink['rate_hz']) if topic_data_uplink['rate_hz'] > 0 else 0
                uplink_topics.append(topic_data_uplink)

                topic_data_downlink = topic_data.copy()
                topic_data_downlink['rate_hz'] = topic.get('downlink_rate_hz', 0)
                topic_data_downlink['interval_us'] = int(1e6 / topic_data_downlink['rate_hz']) if topic_data_downlink['rate_hz'] > 0 else 0
                downlink_topics.append(topic_data_downlink)

            msg_id = topic_data['msg_type_id']
            if msg_id not in seen_ids:
                unique_topics.append(topic_data)
                seen_ids.add(msg_id)

    # Render C++ Converter Headers and Impl
    template_files = [
        ('uorb_to_proto.h.j2', 'uorb_to_proto.h'),
        ('uorb_to_proto.cpp.j2', 'uorb_to_proto.cpp'),
        ('pblink_topics.h.j2', 'pblink_topics.h'),
        ('pblink_downlink.h.j2', 'pblink_downlink.h')
    ]

    for tmpl_name, out_name in template_files:
        tmpl = env.get_template(tmpl_name)
        content = tmpl.render(
            uplink_topics=uplink_topics,
            downlink_topics=downlink_topics,
            unique_topics=unique_topics,
            package_name=package_name
        )
        write_generated(os.path.join(args.generated_dir, out_name), content)
        print(f"Generated {out_name}")

if __name__ == '__main__':
    main()
