import requests, json, re


class ankiWrapper(object):
    def __init__(self, config) -> None:
        self.ankiConnectHost = config["ankiConnectHost"]
        self.ankiConnectPort = config["ankiConnectPort"]

        self.data = {"decks": {}}

        try:
            self.data["version"] = requests.post(
                f"http://{self.ankiConnectHost}:{self.ankiConnectPort}",
                json={"action": "version"},
            ).json()
        except requests.exceptions.ConnectionError:
            raise Exception("Could not connect to AnkiConnect. Is Anki running?")

        self.syncCollection()

    def makeConnectRequest(self, action, params={}):
        try:
            return requests.post(
                f"http://{self.ankiConnectHost}:{self.ankiConnectPort}",
                json={
                    "action": action,
                    "params": params,
                    "version": self.data["version"],
                },
            ).json()["result"]
        except Exception as error:
            return {"result": [], "error": error}

    def syncCollection(self):
        self.makeConnectRequest("sync")
        self.makeConnectRequest("reloadCollection")
        self.getData()

    def getData(self):
        user_decks = self.makeConnectRequest("deckNamesAndIds")

        try:
            del user_decks["Default"]
        except KeyError:
            print("Defualt deck doesn't exist, skipping")

        self.data["cardsReviewed"] = self.makeConnectRequest("getNumCardsReviewedToday")
        self.data["collectionStats"] = self.makeConnectRequest(
            "getCollectionStatsHTML", params={"wholeCollection": True}
        )
        deckStats = self.makeConnectRequest(
            "getDeckStats", params={"decks": list(user_decks.keys())}
        )

        for deck_name in user_decks.keys():
            # deck_cards = self.makeConnectRequest(
            #    "findCards", {"query": f"deck:{deck_name}"}
            # )

            self.data["decks"][deck_name] = {}
            self.data["decks"][deck_name]["stats"] = deckStats[
                str(user_decks[deck_name])
            ]  # Get ID from name
            # self.data["decks"][deck_name]["cards"] = self.makeConnectRequest(
            #    "cardsInfo", {"cards": deck_cards}
            # )

    def selectDeck(self, deck):
        self.makeConnectRequest("guiDeckReview", params={"name": deck})

    def getCurrentCard(self):
        card = self.makeConnectRequest("guiCurrentCard")

        self.makeConnectRequest("guiShowQuestion")
        self.makeConnectRequest("guiStartCardTimer")

        return card

    def showAnswerCard(self):
        self.makeConnectRequest("guiShowAnswer")

    def answerCard(self, choice):
        print(self.makeConnectRequest("guiAnswerCard", params={"ease": int(choice)}))
