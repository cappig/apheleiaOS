#!/bin/sh

room=path
has_key=no
playing=yes
won=no

look() {
    echo
    case "$room" in
        path)
            echo "FOREST PATH"
            echo "An old stone tower stands to the north."
            echo "A ruined shrine lies east through the trees."
            ;;
        shrine)
            echo "RUINED SHRINE"
            echo "A cracked statue of a knight watches over a stone basin."
            if [ "$has_key" = no ]; then
                echo "A small iron key rests inside the basin."
            fi
            echo "The forest path is west."
            ;;
        tower)
            echo "OLD TOWER"
            echo "A wooden chest sits beneath a faded dragon banner."
            echo "The forest path is south."
            ;;
    esac
}

move() {
    case "$room:$1" in
        path:north|path:n)
            room=tower
            look
            ;;
        path:east|path:e)
            room=shrine
            look
            ;;
        shrine:west|shrine:w)
            room=path
            look
            ;;
        tower:south|tower:s)
            room=path
            look
            ;;
        *)
            echo "You cannot go that way."
            ;;
    esac
}

take_key() {
    if [ "$room" != shrine ]; then
        echo "There is no key here."
    elif [ "$has_key" = yes ]; then
        echo "You already have the key."
    else
        has_key=yes
        echo "You pick up the iron key."
    fi
}

open_chest() {
    if [ "$room" != tower ]; then
        echo "There is no chest here."
    elif [ "$has_key" = no ]; then
        echo "The chest is locked."
    else
        echo "The key fits. The chest creaks open..."
        sleep 1 &
        wait "$!"
        echo "Inside is the stolen Bell of Bramblewick!"
        echo "You ring it. Somewhere, a goblin says a very bad word."
        won=yes
        playing=no
    fi
}

show_help() {
    echo "Commands: look, north, south, east, west"
    echo "          take key, open chest, inventory, quit"
}

echo
echo "THE BELL OF BRAMBLEWICK"
echo "A goblin stole the village bell and hid it in the old tower."
echo "Bring it home, $(whoami)."
show_help
look

while [ "$playing" = yes ]; do
    echo
    echo "What now?"
    read verb noun

    case "$verb" in
        look|l) look ;;
        north|n|south|s|east|e|west|w) move "$verb" ;;
        take|get)
            if [ "$noun" = key ]; then
                take_key
            else
                echo "You cannot take that."
            fi
            ;;
        open)
            if [ "$noun" = chest ]; then
                open_chest
            else
                echo "You cannot open that."
            fi
            ;;
        inventory|i)
            if [ "$has_key" = yes ]; then
                echo "You are carrying a small iron key."
            else
                echo "You are carrying nothing."
            fi
            ;;
        help|h) show_help ;;
        quit|q) playing=no ;;
        *) echo "Try 'help'." ;;
    esac
done

echo
if [ "$won" = yes ]; then
    echo "Quest complete."
else
    echo "The bell will have to wait."
fi
