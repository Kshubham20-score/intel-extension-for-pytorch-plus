import json
from ruamel.yaml import YAML
import os
import sys
import shutil
import re

yaml = YAML()

def load_from_yaml(yaml_file):
    with open(yaml_file, "r") as load_f:
        loaded = yaml.load(load_f.read())
    return loaded

def save_to_yaml(src, target_file):
    with open(target_file, "w") as save_f:
        yaml.dump(src, save_f)

def load_from_json(json_file):
    with open(json_file, "r") as load_f:
        loaded = json.load(load_f)
    return loaded


def save_to_json(src, target_file):
    data = json.dumps(src, indent=2)
    with open(target_file, "w", newline="\n") as save_f:
        save_f.write(data)


def read_file(file_name):
    f = open(file_name, "r")
    data = f.read()
    f.close()
    return data

def read_file_lines(file_name):
    f = open(file_name, "r")
    lines = f.readlines()
    f.close()
    return lines

def write_file(data, file_name, mode="w", end_char=""):
    if not isinstance(file_name, str):
        print(data + end_char, file=file_name)
        return
    dirname = os.path.dirname(os.path.realpath(file_name))
    if not os.path.exists(dirname):
        os.makedirs(dirname)
    f = open(file_name, mode)
    f.write(data + end_char)
    f.close()

def write_file_lines(lines, file_name, mode="w", end_char=""):
    if not isinstance(file_name, str):
        for line in lines:
            print(line + end_char, file=file_name)
        return
    dirname = os.path.dirname(os.path.realpath(file_name))
    if not os.path.exists(dirname):
        os.makedirs(dirname)
    f = open(file_name, mode)
    lines = [line + end_char for line in lines]
    f.writelines(lines)
    f.close()

def insert_header_lines(data, file_name):
    old_data = read_file(file_name)
    new_data = data + old_data
    write_file(new_data, file_name)

def insert_main_lines(data, file_name):
    lines = read_file_lines(file_name)
    for i, line in enumerate(lines):
        if re.match("if __name__ == .__main__.:", line, re.S | re.I):
            indent = len(lines[i+1]) - len(lines[i+1].lstrip())
            new_line = " " * indent + data;
            lines.insert(i + 1, new_line)
    write_file_lines(lines, file_name)

def copy_dir_or_file(src_path, tgt_path):
    src_full_path = os.path.realpath(src_path)
    tgt_full_path = os.path.realpath(tgt_path)
    # copy directory (NOT contain files/dirs under this directory)
    if os.path.isdir(src_full_path) and not os.path.exists(tgt_full_path):
        os.makedirs(tgt_full_path)
    # copy file
    elif os.path.isfile(src_full_path):
        tgt_base_dir = os.path.dirname(tgt_full_path)
        if not os.path.exists(tgt_base_dir):
            os.makedirs(tgt_base_dir)
        shutil.copyfile(src_full_path, tgt_full_path)

def remove_files(files_list):
    for f in files_list:
        if not os.path.exists(f):
            continue
        assert os.path.isfile(f), "[ERROR] remove_files can only remove files, not dirs"
        os.remove(f)

def touch_init_py(root_dir):
    for root, dirs, files in os.walk(root_dir):
        for d in dirs:
            init_py = os.path.join(root, d, "__init__.py")
            if not os.path.exists(init_py):
                os.system(f"touch {init_py}")
            assert os.path.isfile(init_py), f"Touch {init_py} FAILED."
