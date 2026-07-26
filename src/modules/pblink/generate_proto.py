#!/usr/bin/env python3
"""
generate_proto.py - Developer CLI Tool

Converts PX4 uORB .msg files into Protobuf .proto files and exports
them directly into the pblink-proto repository directory.
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

def main():
    parser = argparse.ArgumentParser(description="Developer Tool: Generate Protobuf .proto files from uORB .msg files.")
    parser.add_argument('-y', '--yaml-file', required=True, help="Path to topics.yaml")
    parser.add_argument('-m', '--msg-dir', required=True, help="Path to PX4 msg directory")
    parser.add_argument('-t', '--template-dir', required=True, help="Path to Jinja2 templates directory")
    parser.add_argument('-o', '--output-dir', help="Output directory for .proto files (defaults to pblink-proto/proto)")

    args = parser.parse_args()

    with open(args.yaml_file, 'r') as f:
        config = yaml.safe_load(f)

    package_name = config.get('package', 'px4_pblink.msgs')
    topics = config.get('topics', [])

    env = Environment(loader=FileSystemLoader(args.template_dir))
    env.filters['uorb_to_proto_type'] = uorb_to_proto_type
    proto_template = env.get_template('proto.j2')

    output_dir = args.output_dir
    if not output_dir:
        output_dir = os.path.join(os.path.dirname(args.yaml_file), "generated")

    os.makedirs(output_dir, exist_ok=True)

    generated_count = 0
    for topic in topics:
        topic_name_snake = topic['name']
        topic_name_camel = snake_to_camel(topic_name_snake)

        custom_proto = os.path.join(output_dir, f"{topic_name_snake}.proto")
        if topic.get('internal', False) and os.path.exists(custom_proto):
            print(f"Skipping custom RPC topic: {topic_name_snake}")
            continue

        msg_file_path = find_msg_file(args.msg_dir, topic_name_camel)
        if not msg_file_path:
            continue

        fields = parse_msg_file(msg_file_path)
        proto_content = proto_template.render(
            package_name=package_name,
            message_name=topic_name_camel,
            fields=fields
        )

        proto_output_path = os.path.join(output_dir, f"{topic_name_snake}.proto")
        write_generated(proto_output_path, proto_content)
        print(f"Generated {proto_output_path}")
        generated_count += 1

    print(f"\nSuccessfully exported {generated_count} .proto files to {output_dir}")

if __name__ == '__main__':
    main()
