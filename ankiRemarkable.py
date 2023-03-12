from carta import ReMarkable, Widget


class rmWrapper(object):
    def __init__(self, config) -> None:
        self.config = config

        self.rm = ReMarkable(
            simple=self.config["simple"], rm2fb=self.config["rm2fb"], debug=True
        )
        self.rm.timeout = self.config["timeout"]
        self.ghosting_count = 0

        self.createButtons()
        self.resetScreen()

    def createButtons(self):
        self.again = Widget(
            id="1",
            typ="button",
            value="Again",
            x=280,
            y=1670,
            width=200,
            height=80,
            fontsize=65,
        )
        self.hard = Widget(
            id="2",
            typ="button",
            value="Hard",
            x=540,
            y=1670,
            width=145,
            height=80,
            fontsize=65,
        )
        self.good = Widget(
            id="3",
            typ="button",
            value="Good",
            x=770,
            y=1670,
            width=120,
            height=80,
            fontsize=65,
        )
        self.easy = Widget(
            id="4",
            typ="button",
            value="Easy",
            x=1006,
            y=1670,
            width=120,
            height=80,
            fontsize=65,
        )
        self.reveal = Widget(
            id="reveal",
            typ="button",
            value="Show answer",
            x=f"50%",
            y=f"90%",
            width=390,
            height=80,
            fontsize=65,
            justify="center",
        )

    def getHost(self):  # TODO: Implement this
        self.rm.add(
            Widget(
                id="ankihost",
                typ="label",
                value="",
                x="20%",
                y="20%",
                fontsize=70,
                justify="left",
            )
        )
        self.rm.add(
            Widget(
                id="ankihost_input",
                typ="textinput",
                value="Enter connect host:",
                x="20%",
                y="20%",
                fontsize=70,
            )
        )
        self.rm.add(
            Widget(id="ankiport", typ="label", value="", x="20%", y="20%", fontsize=70)
        )
        self.rm.add(
            Widget(
                id="ankiport_input",
                typ="textinput",
                value="Enter connect port:",
                x="20%",
                y="30%",
                fontsize=70,
            )
        )

    def generateMenu(self):
        self.rm.add(
            Widget(
                id="left_arrow",
                typ="image",
                value="assets/arrow-left.png",
                x=1040,
                y=44,
                width=80,
                height=80,
            )
        )
        self.rm.add(
            Widget(
                id="right_arrow",
                typ="image",
                value="assets/arrow-right.png",
                x=1220,
                y=44,
                width=80,
                height=80,
            )
        )
        self.rm.add(
            Widget(
                id="page",
                typ="label",
                value="1/2",
                x=1130,
                y=60,
                justify="center",
                fontsize="48",
            )
        )
        self.rm.add(
            Widget(
                id="sync_button",
                typ="image",
                value="assets/upload-cloud.png",
                x=1240,
                y=1801,
                width=50,
                height=50,
            )
        )

        for i in range(0, 50):
            self.rm.add(
                Widget(
                    id="vert_line" + str(i),
                    typ="label",
                    value="|",
                    x=1040,
                    y=i + 1775,
                    justify="left",
                    fontsize="50",
                )
            )

        self.rm.add(
            Widget(
                id="sync",
                typ="button",
                value="Sync",
                x=1120,
                y=1801,
                justify="center",
                fontsize="30",
            )
        )

    def generateDeckList(self, decks):
        y_pos = 180

        deck_list = list(decks.keys())  # Dicts are ordered so need to make it a list

        for deck, deck_data in decks.items():
            x_pos = 100

            deck = deck.split("::")

            if len(deck) > 1:
                x_pos += 75
            value = deck[-1]

            self.rm.add(
                Widget(
                    id=value,  # TODO: Carta OR simple breaks with ::
                    typ="label",
                    value=value,
                    x=x_pos,
                    y=y_pos,
                    justify="left",
                    fontsize="38",
                )
            )
            y_pos += 37
            self.rm.add(
                Widget(
                    id=f"{value} stats",
                    typ="label",
                    value=f"{deck_data['stats']['review_count']} cards to review - {deck_data['stats']['total_in_deck']} cards",
                    x=x_pos,
                    y=y_pos,
                    justify="left",
                    fontsize="38",
                )
            )
            self.rm.add(
                Widget(
                    id=f"{'=='.join(deck)} select",
                    typ="image",
                    value="assets/chevron-right.png",
                    x=1240,
                    y=y_pos - 35,
                    height=75,
                    width=65,
                    justify="left",
                    fontsize="38",
                )
            )
            pos = deck_list.index("::".join(deck))
            if pos + 1 < len(deck_list):
                if deck_list[pos + 1].split("::")[0] != deck[0]:
                    y_pos += 45
                    self.rm.add(
                        Widget(
                            id=f"{deck[-1]} split",
                            typ="label",
                            value="_" * 200,
                            x=0,
                            y=y_pos,
                            fontsize=20,
                            justify="left",
                        )
                    )
                    y_pos -= 15
            y_pos += 60

    def display(self):
        self.ghosting_count += 1
        
        if self.ghosting_count > self.config['ghosting']:
            self.rm.eclear()
            self.ghosting_count = 0 # Reset ghosting count
        
        try:
            choice = self.rm.display()

            if choice[1] == True:
                return choice[0]
            return choice

        except Exception as error:
            raise Exception(f"Could not display screen: {error}")    
        
    def resetScreen(self):
        self.rm.screen = [
            Widget(
                id="title",
                typ="label",
                value="reMarkable Anki",
                x=100,
                y=60,
                fontsize="48",
                justify="left",
            ),
            Widget(
                id="title_line",
                typ="label",
                value="_" * 150,
                x=0,
                y=106,
                justify="left",
                fontsize="48",
            ),
            Widget(
                id="bottom_bar",
                typ="label",
                value="_" * 150,
                x=0,
                y=1740,
                justify="left",
                fontsize="48",
            ),
            Widget(
                id="version",
                typ="label",
                value="v1.0.0 - @Jayy001",
                x=100,
                y=1810,
                justify="left",
                fontsize="30",
            ),
        ]

    def generateCard(self, cardId, question, deck):
        self.resetScreen()
        self.rm.lookup("version").value = f"v0.1.6 - @Jayy001 - {deck}"

        card = Widget(
            id=cardId,
            typ="paragraph",
            value=f"{question}",
            x=100,
            y=245,
            fontsize=70,
            height=500,
            width=1255,
        )
        self.rm.add(card, self.reveal)
    
    def modifyQuestionValue(self, cardId, replacement):
        self.rm.lookup(cardId).value = replacement
        print(replacement, 'hello')

    def generateAnswer(self, cardId, answer):
        card = self.rm.lookup(cardId)

        card_line = Widget(
            id=f"{str(cardId)}_line",
            typ="paragraph",
            value=f"{' '*len(card.value)}\n\n\n{'_'*86}",
            x=98,
            y=245,
            fontsize=27,
            height=500,
            width=1255,
        )
        card.value = f"{card.value}\n\n{answer}"

        self.rm.remove(widget=self.reveal)
        self.rm.add(card_line, self.again, self.hard, self.good, self.easy)
