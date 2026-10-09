# Interactive shells on the bench. Other parts of the bench add their own /etc/profile.d snippets
# (the audio stack's, for instance).

case $- in
    *i*) ;;
    *) return 0 ;;
esac

# The hostname in the prompt: with several benches on the desk, it says which one this is.
PS1='\u@\h:\w\$ '

alias ll='ls -l'
alias la='ls -lA'
