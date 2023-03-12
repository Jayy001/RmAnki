#!/opt/bin/python3

import json, sys, re

from ankiConnect import ankiWrapper
from ankiRemarkable import rmWrapper

from copy import copy

with open(f"assets/{sys.argv[1]}") as config_file:
    config = json.load(config_file)

anki = ankiWrapper(config)
rm = rmWrapper(config)


def generateMainMenu():
    rm.resetScreen()
    rm.generateMenu()
    rm.generateDeckList(anki.data["decks"])


def run():
    choice = rm.display()

    if choice == "right_arrow":  # TODO: Add a paginator
        return
    elif choice == "left_arrow":
        return
    elif choice == "sync":
        anki.syncCollection()
        generateMainMenu()
    else:  # Deck selected
        choice = choice.replace("==", "::")[:-7]
        anki.selectDeck(choice)


        while True:
            card = anki.getCurrentCard()
            
            if card is None:
                generateMainMenu()
                break # Finished deck
            
            cardId = str(card["cardId"])
            deck = card["deckName"]

            if "cloze" in card["modelName"]:
                cloze = card["fields"]["Text"]["value"]
                question = re.sub(r"\{\{c\d+::\w+\}\}", "_"*5, cloze)
                answer = card["fields"]["Back Extra"]["value"]
            else:
                question = card["fields"]["Front"]["value"]
                answer = card["fields"]["Back"]["value"]

            rm.generateCard(cardId, question, deck)
            rm.display()

            anki.showAnswerCard()
            rm.generateAnswer(cardId, answer)
            
            if "cloze" in card["modelName"]:
                rm.modifyQuestionValue(cardId, cloze)   

            confidence = rm.display()
            anki.answerCard(confidence)


if __name__ == "__main__":
    generateMainMenu()

    while True:
        run()
