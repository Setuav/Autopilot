#!/usr/bin/env python3
"""
generate_cpp.py - PX4 Build Generator Script

Executed during CMake build time (`make px4_sitl_pblink`).
Reads topics.yaml and pre-existing .proto files from proto/custom/ and proto/generated/,
renders Jinja2 templates, and outputs C++ converter headers/sources.
"""
import os
import argparse
import re
import yaml
import shutil
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
    candidate_versioned = os.path.join(msg_dir, "versioned", f"{topic_name_pascal}.msg")
    if os.path.exists(candidate_versioned):
        return candidate_versioned
    return None

def write_generated(filepath, content):
    os.makedirs(os.path.dirname(filepath), exist_ok=True)
    if not content.endswith('\n'):
        content += '\n'
    if os.path.exists(filepath):
        with open(filepath, 'r') as f:
            if f.read() == content:
                return
    with open(filepath, 'w') as f:
        f.write(content)

def process_topic(topic, msg_dir, proto_dir, yaml_file_path):
    topic_name_snake = topic['name']
    topic_name_camel = snake_to_camel(topic_name_snake)
    is_internal = topic.get('internal', False)

    # Copy pre-existing .proto file from custom/ or generated/ to proto_dir for Nanopb
    custom_proto_path = os.path.join(os.path.dirname(yaml_file_path), "custom", f"{topic_name_snake}.proto")
    if not os.path.exists(custom_proto_path):
        custom_proto_path = os.path.join(os.path.dirname(yaml_file_path), "generated", f"{topic_name_snake}.proto")

    if os.path.exists(custom_proto_path):
        proto_output_path = os.path.join(proto_dir, f"{topic_name_snake}.proto")
        if os.path.abspath(custom_proto_path) != os.path.abspath(proto_output_path):
            shutil.copyfile(custom_proto_path, proto_output_path)
        custom_options_path = custom_proto_path.replace(".proto", ".options")
        if os.path.exists(custom_options_path):
            options_output_path = os.path.join(proto_dir, f"{topic_name_snake}.options")
            if os.path.abspath(custom_options_path) != os.path.abspath(options_output_path):
                shutil.copyfile(custom_options_path, options_output_path)

    # For internal RPC topics, no uORB msg parsing is needed
    if is_internal:
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
            'internal': True
        }

    # For uORB topics, parse fields from .msg file for C++ converter generation
    fields = []
    if 'msg_file' in topic:
        explicit_filename = topic['msg_file']
        possible_paths = [
            os.path.join(os.path.dirname(yaml_file_path), explicit_filename),
            os.path.join(os.path.dirname(yaml_file_path), "custom", explicit_filename),
            os.path.join(msg_dir, explicit_filename) if msg_dir else "",
            os.path.join(msg_dir, "versioned", explicit_filename) if msg_dir else ""
        ]

        msg_file_path = None
        for path in possible_paths:
            if path and os.path.exists(path):
                msg_file_path = path
                break

        if msg_file_path:
            fields = parse_msg_file(msg_file_path)
    else:
        msg_file_path = find_msg_file(msg_dir, topic_name_camel)
        if msg_file_path:
            fields = parse_msg_file(msg_file_path)

    rate_hz = topic.get('rate_hz', 0)
    interval_us = int(1e6 / rate_hz) if rate_hz > 0 else 0

    return {
        'name': topic_name_snake,
        'name_pascal': topic_name_camel,
        'name_upper': topic_name_snake.upper(),
        'fields': fields,
        'msg_type_id': topic.get('msg_type_id', 0),
        'rate_hz': rate_hz,
        'interval_us': interval_us,
        'description': topic.get('description', ''),
        'internal': False
    }

def main():
    parser = argparse.ArgumentParser(description="PX4 Build Tool: Generate C++ converter files from Protobuf and uORB schemas.")
    parser.add_argument('-y', '--yaml-file', required=True, help="YAML config file")
    parser.add_argument('-m', '--msg-dir', help="PX4 msg directory")
    parser.add_argument('-t', '--template-dir', help="Jinja2 templates directory")
    parser.add_argument('-p', '--proto-dir', help="Output directory for .proto files")
    parser.add_argument('-g', '--generated-dir', help="Output directory for C++ converter files")
    parser.add_argument('--list-topics', action='store_true', help="Print topic names from the YAML config and exit")

    args = parser.parse_args()

    with open(args.yaml_file, 'r') as f:
        config = yaml.safe_load(f)

    if args.list_topics:
        topic_names = [t['name'] for t in config.get('topics', []) if 'name' in t]
        print(";".join(topic_names))
        return

    env = Environment(
        loader=FileSystemLoader(args.template_dir),
        trim_blocks=True,
        lstrip_blocks=True
    )
    env.filters['uorb_to_proto_type'] = uorb_to_proto_type
    env.filters['capitalize'] = snake_to_camel

    converter_h_template = env.get_template('converter_h.j2')
    converter_cpp_template = env.get_template('converter_cpp.j2')
    topics_header_template = env.get_template('topics_header.j2')
    downlink_handlers_template = env.get_template('downlink_handlers.j2')
    uplink_senders_template = env.get_template('uplink_senders.j2')

    package_name = config.get('package', 'px4_pblink.msgs')

    uplink_topics = []
    downlink_topics = []
    bidirectional_topics = []

    for topic in config.get('topics', []):
        direction = topic.get('direction', 'uplink')

        topic_data = process_topic(topic, args.msg_dir, args.proto_dir, args.yaml_file)
        if not topic_data:
            continue

        topic_data['direction'] = direction

        if direction == 'uplink':
            uplink_topics.append(topic_data)
        elif direction == 'downlink':
            downlink_topics.append(topic_data)
        elif direction == 'both':
            uplink_copy = topic_data.copy()
            downlink_copy = topic_data.copy()

            uplink_copy['direction'] = 'uplink'
            downlink_copy['direction'] = 'downlink'

            if 'uplink_rate_hz' in topic:
                uplink_rate = topic['uplink_rate_hz']
                uplink_copy['rate_hz'] = uplink_rate
                uplink_copy['interval_us'] = int(1e6 / uplink_rate) if uplink_rate > 0 else 0

            if 'downlink_rate_hz' in topic:
                downlink_rate = topic['downlink_rate_hz']
                downlink_copy['rate_hz'] = downlink_rate
                downlink_copy['interval_us'] = int(1e6 / downlink_rate) if downlink_rate > 0 else 0

            uplink_topics.append(uplink_copy)
            downlink_topics.append(downlink_copy)
            bidirectional_topics.append(topic_data)

    all_topics = uplink_topics + downlink_topics

    if all_topics:
        h_content = converter_h_template.render(
            package_name=package_name,
            topics=all_topics,
            uplink_topics=uplink_topics,
            downlink_topics=downlink_topics
        )
        h_output_path = os.path.join(args.generated_dir, "uorb_to_proto.h")
        write_generated(h_output_path, h_content)

        cpp_content = converter_cpp_template.render(
            package_name=package_name,
            topics=all_topics,
            uplink_topics=uplink_topics,
            downlink_topics=downlink_topics
        )
        cpp_output_path = os.path.join(args.generated_dir, "uorb_to_proto.cpp")
        write_generated(cpp_output_path, cpp_content)

        unique_topics = []
        seen_ids = set()
        for topic in all_topics:
            if topic['msg_type_id'] not in seen_ids:
                unique_topics.append(topic)
                seen_ids.add(topic['msg_type_id'])

        topics_h_content = topics_header_template.render(
            package_name=package_name,
            uplink_topics=uplink_topics,
            downlink_topics=downlink_topics,
            unique_topics=unique_topics
        )
        topics_h_output_path = os.path.join(args.generated_dir, "pblink_topics.h")
        write_generated(topics_h_output_path, topics_h_content)

        downlink_h_content = downlink_handlers_template.render(
            package_name=package_name,
            downlink_topics=downlink_topics
        )
        downlink_h_output_path = os.path.join(args.generated_dir, "pblink_downlink.h")
        write_generated(downlink_h_output_path, downlink_h_content)

        uplink_h_content = uplink_senders_template.render(
            package_name=package_name,
            uplink_topics=uplink_topics
        )
        uplink_h_output_path = os.path.join(args.generated_dir, "pblink_uplink.h")
        write_generated(uplink_h_output_path, uplink_h_content)

    print(f"Generated C++ converter files for {len(uplink_topics)} uplink topics and {len(downlink_topics)} downlink topics.")

if __name__ == '__main__':
    main()
