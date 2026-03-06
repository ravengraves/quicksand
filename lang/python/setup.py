import os
import sys
from setuptools import setup, Extension, find_packages

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

extra_compile_args = ["-Os"]
extra_link_args = []

if sys.platform.startswith("linux"):
    extra_compile_args.extend(["-flto", "-march=native", "-mtune=native"])
    extra_link_args.append("-ldl")
elif sys.platform == "darwin":
    extra_compile_args.append("-flto")

_quicksand_ext = Extension(
    name="quicksand._quicksand",
    sources=["quicksand/_quicksand.c"],
    include_dirs=[os.path.join(ROOT, "include")],
    extra_link_args=extra_link_args,
    extra_compile_args=extra_compile_args,
)

setup(
    name="quicksand",
    version="1.0.0",
    description="Python bindings for the quicksand message passing library",
    author="Alec Graves",
    packages=find_packages(),
    ext_modules=[_quicksand_ext],
    # package_data={
    #     "quicksand": ["libquicksand.so"]
    # },
    # include_package_data=True,
    zip_safe=True,
    python_requires=">=3.9",
)
