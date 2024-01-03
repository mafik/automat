import subprocess


def query_default_defines() -> set[str]:
    result = subprocess.run(['clang', '-dM', '-E', '-'],
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE)
    return set([line.split()[1] for line in result.stdout.decode().splitlines()])


default_defines = query_default_defines()
