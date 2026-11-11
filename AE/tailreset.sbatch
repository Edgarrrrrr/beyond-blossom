#!/bin/bash
#SBATCH --partition=debug
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=52
#SBATCH --mem=100G
#SBATCH --exclusive
#SBATCH --exclude=ares7,ares8
set -uo pipefail
AE_TAILRESET_RUN=$1
shift
trap 'AE_TAILRESET_CODE=$?; printf "%s\n" "$AE_TAILRESET_CODE" > "$AE_TAILRESET_RUN/exit-${SLURM_JOB_ID}.txt"' EXIT
case "$(hostname -s)" in ares[1-6]) ;; *) exit 90 ;; esac
export PATH=/usr/bin:/bin:/usr/local/bin
export PYTHONDONTWRITEBYTECODE=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 NUMEXPR_NUM_THREADS=1
"$@"
