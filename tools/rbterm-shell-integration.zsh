# rbterm shell integration (zsh).
#
# Emits OSC 133 prompt marks so rbterm can:
#   - paint a green/red gutter badge next to each command
#   - (future) jump to prev/next prompt with a chord
#
# Source this once from ~/.zshrc:
#     source /path/to/rbterm-shell-integration.zsh
#
# Idempotent: re-sourcing the file is safe.

# Skip if not interactive or not actually inside a terminal.
[[ -o interactive ]] || return 0
[[ -t 1 ]]          || return 0

# Internal: emit OSC 133;A (prompt about to draw) right before $PS1
# is rendered. Stash the previous command's $? so D can carry it.
_rbterm_precmd() {
    local _ec=$?
    print -Pn "\e]133;D;${_ec}\e\\"
    print -Pn "\e]133;A\e\\"
}

# Internal: emit OSC 133;C (command output starts) right after the
# user hits Enter, before the command runs.
_rbterm_preexec() {
    print -Pn "\e]133;C\e\\"
}

# Hook into zsh's standard arrays (idempotent — won't double-add).
typeset -ga precmd_functions preexec_functions
(( ${precmd_functions[(I)_rbterm_precmd]} ))   || precmd_functions+=(_rbterm_precmd)
(( ${preexec_functions[(I)_rbterm_preexec]} )) || preexec_functions+=(_rbterm_preexec)
