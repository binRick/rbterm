# rbterm shell integration (bash).
#
# Emits OSC 133 prompt marks so rbterm can paint gutter badges
# (green = exit 0, red = nonzero) next to every command in the
# scrollback. Source this once from ~/.bashrc:
#
#     source /path/to/rbterm-shell-integration.bash
#
# Idempotent: re-sourcing the file is safe.

# Skip non-interactive shells / non-tty stdout.
case $- in *i*) ;; *) return 0;; esac
[[ -t 1 ]] || return 0

# Emit OSC 133;D (with $? from the last command) and 133;A right
# before each prompt is drawn. PROMPT_COMMAND is bash's pre-prompt
# hook; we prepend so the marks land before the user's own prompt
# customisation runs.
_rbterm_precmd() {
    local ec=$?
    printf '\e]133;D;%d\e\\\e]133;A\e\\' "$ec"
}

# Emit OSC 133;C right after the user hits Enter (DEBUG trap fires
# before each command). Skip when the trap is firing for the
# PROMPT_COMMAND chain itself — we only want it for user commands.
_rbterm_preexec() {
    [[ "$BASH_COMMAND" == "_rbterm_precmd"* ]] && return
    printf '\e]133;C\e\\'
}

# Wire up the hooks idempotently.
case ";${PROMPT_COMMAND};" in
    *";_rbterm_precmd;"*) ;;
    *) PROMPT_COMMAND="_rbterm_precmd${PROMPT_COMMAND:+;$PROMPT_COMMAND}" ;;
esac
trap '_rbterm_preexec' DEBUG
