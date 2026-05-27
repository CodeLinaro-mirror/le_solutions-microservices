#!/bin/bash

# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# Some more ls aliases
alias ls='ls --color=auto'
alias ll='ls -alF'
alias la='ls -A'
alias l='ls -CF'

# Enable bash completion in interactive shells
[ -f /etc/bash_completion ] && . /etc/bash_completion

[ -f ${LLM_SCRIPTS_DIR}/setup.sh ] && source ${LLM_SCRIPTS_DIR}/setup.sh
[ -f ${CHATCOMPLETIONS_SCRIPTS_DIR}/setup.sh ] && source ${CHATCOMPLETIONS_SCRIPTS_DIR}/setup.sh
