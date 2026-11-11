"""Console entry point with conventional process status and error output."""
import sys

import beyond_blossom


def cli_argv():
    try:
        return beyond_blossom.cli(command_line_args=sys.argv[1:])
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
