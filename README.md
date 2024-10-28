# statphysbio dssp

This is a fork of [PDB-REDO/dssp](https://github.com/PDB-REDO/dssp?tab=readme-ov-file) that we distribute through the statphysbio anaconda channel.

## Installation
```
conda install dssp -c statphysbio
```

## Release
To make a new conda release (for example, to add support for x86 macOS, Windows, etc):

1. Get  a computer with the architecture/OS you want to target
2. Find the statphysbio anaconda API key (https://anaconda.org/statphysbio/dashboard > statphybio [top right] > settings > access > scroll to GitHubActions at the bottom > view)
3. Clone this repo and cd into it
4. 
```
export ANACONDA_API_TOKEN="XXXX"
conda install conda-build anaconda-client
conda build devtools/conda-build --no-test --output-folder ./build -c conda-forge
anaconda upload --user "statphysbio" ./build/{your arch}/dssp-{version}.tar.bz
```