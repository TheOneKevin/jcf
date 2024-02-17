import os
import subprocess

def grab_all_java(dir):
    # Recursively grab all java files
    java_files = []
    for root, _, files in os.walk(dir):
        for file in files:
            if file.endswith(".java"):
                java_files.append(os.path.join(root, file))
    return java_files

# Get the directory of this script
script_dir = os.path.dirname(os.path.realpath(__file__))

# Set up the test directories
test_dir = "/u/cs444/pub/assignment_testcases/a2/"
stdlib_dir = "/u/cs444/pub/stdlib/2.0"
joosc = os.path.join(script_dir, "build", "joosc")

# List test cases files
test_names = [ os.path.join(test_dir, f) for f in os.listdir(test_dir) ]
stdlib_files = grab_all_java(stdlib_dir)

# Deliniate the valid and invalid files
invalid_files = [x for x in test_names if x.startswith("Je_")]
valid_files = [x for x in test_names if x not in invalid_files]

def run_on_files(files):
    ret = subprocess.run([joosc, *files], stderr=subprocess.PIPE, stdout=subprocess.PIPE)
    return ret.returncode, ret.stdout, ret.stderr

# Test the valid files now

failures = 0

for test in valid_files:
    print(f"Running test on: {test}")
    files = [test] if os.path.isfile(test) else grab_all_java(test)
    ret, x, y = run_on_files(files)
    if ret != 0:
        print("Test failed")
        failures += 1
        print(f"Command: {joosc} {' '.join(files)}")
        # print(f"Return code: {ret}")
        # print(f"Stdout: {x}")
        # print(f"Stderr: {y}")
        # exit(0)

for test in invalid_files:
    print(f"Running test on: {test}")
    ret, x, y = run_on_files([test] if os.path.isfile(test) else grab_all_java(test))
    if ret != 42:
        print("Test failed")
        failures += 1
        print(f"Command: {joosc} {' '.join(files)}")

print(f"Total failures: {failures}/{len(valid_files) + len(invalid_files)}")
