# Apparence native en partie (v6)

L'equipement reste dans le menu natif de Dota. Le recepteur publie une selection
immuable apres livraison de la derniere reponse Equip. Le ClientThink du heros
local regroupe les changements sur 75 ms. Une tenue identique ne declenche pas
de reconstruction ; les objets absents autorisent quatre tentatives espacees.

La creation des accessoires et la combinaison de modeles sont deux etapes
distinctes. Remplacer uniquement les entrees du combineur laisse les accessoires
par defaut ou attache une persona au mauvais squelette. La v6 prepare donc la
liste native d'accessoires, genere leurs attributs visuels avec le moteur,
selectionne le modele de base, puis recree les accessoires. Le combineur natif
relit les entites creees, deja filtrees selon la persona.

Les appels d'inventaire sont rediriges uniquement dans ce contexte natif. Un
emplacement TLS Windows conserve la selection de la reconstruction en cours ;
un nouvel Equip ne peut pas melanger deux selections dans la meme tenue. Le
compte Steam et le handle complet du heros sont verifies. Pendant le spawn,
l'identifiant du proprietaire fourni par Dota doit correspondre au joueur local.

La tenue precedente reste visible pendant la preparation. Les demandes de
modeles sont reparties sur les frames (deux par frame) ; leur disponibilite
est verifiee sans attente bloquante. Une selection plus recente annule la
preparation precedente. Apres dix secondes sans ressources disponibles,
l'ancienne tenue est conservee jusqu'au prochain equipement.

Un modele qu'aucun serveur n'a precache (le squelette d'une persona, ou un
accessoire equipe en cours de partie) n'existe pas encore pour le systeme de
ressources : `FindOrLoadModel` cree alors un handle vide qui s'affiche comme
`models/dev/error.vmdl`. Comme le combineur natif, la v6 enregistre d'abord
chaque modele inconnu dans le manifest `JustInTimeManifest` de
`IResourceSystem` (etat, enregistrement ou recherche du handle), puis charge
le modele. Les autres remplacements `entity_model` de la tenue (forme de
dragon d'une persona, par exemple) sont enregistres et charges en meme temps,
car une transformation passe aussi par `SetModel(chemin)`. Deux secondes
apres un commit, le nom du modele reellement rendu est relu et affiche (HUD
et journal, champ `render=`), puis verifie toutes les 500 ms : le serveur
reste proprietaire du modele reseau, et la fin d'une transformation ou une
mise a jour complete de l'entite peut remettre le modele classique. Dans ce
cas la tenue est re-appliquee par le chemin normal, au plus trois fois par
tenue (`resyncs=` dans le journal).

Les vues, listes et references de ressources remplacees sont liberees par le
moteur. Le modele original est capture avant la construction des accessoires.
Le retour a une tenue classique restaure le modele visible ET la reference du
combineur ; conserver la reference de la persona melangeait deux squelettes.
Les objets du catalogue ont une position d'inventaire stable non nulle pour
eviter la notification de nouveaux objets a chaque reponse Equip.

## Profil binaire

Installation analysee : `game/dota/bin/win64/client.dll`, 106842264 octets.

- SHA-256 : `d83771b4f61c2b7d882a0fcfc06f83b8d59b11af6f3afd4a1f85d68a449cad78`
- Timestamp PE : `0x6a9b34c3` ; SizeOfImage : `0x6da1000`.
- ClientThink : `0x1ac49f0`.
- Liste d'accessoires : `0x16362a0` ; liste du spawn : `0x16357f0`.
- Preparation : `0x1636ce0` ; creation : `0x16555c0` ; suppression : `0x18fcb10`.
- Lookup joueur : `0x32802d0` ; emplacement : `0x3280760` ; ID : `0x32ebba0`.
- Inventaire local : `0x6424750` ; manager : `0x6424630`.
- Attributs de tenue : constructeur `0x32b83f0`, population `0x32d44b0`.
- Remplacement du modele : `0x163cf30`, application du modele : `0x17497b0`.
- Modele du spawn : `0x618a50` ; reference du combineur : `0x1af97a0`.
- Disponibilite du modele : `0x61fde0` ; modele d'une vue : `0x3325f40`.
- Remplacements `entity_model` d'une liste d'attributs : `0x103fca0`.
- Systeme de ressources : `0x65a7f10` (etat slot 51, enregistrement slot 41,
  recherche slot 79) ; nom de ressource : `0x3a1ad30`, type : `0x3a1a2e0` ;
  chaine `JustInTimeManifest` : `0x3bdac30` ; purge via `tier0!CBufferString::Purge`.
- Controleur local : `0x618ce50` ; handle du heros : controleur + `0x90c`.
- Identites : `0x5df25e0`, pas de `0x70`, numero complet a + `0x10`.

Le hash du fichier, les champs PE en memoire et les entrees des fonctions
profilees sont verifies avant installation. Une autre version de Dota desactive
cette integration. Aucun `offsets.bin` ou ancien writer TCP/keeper n'est utilise.

## Validation

`tests/run_tests.bat` couvre le protocole, les reponses du recepteur, la restauration
des selections, la separation des comptes, les nouvelles creations de heros,
la selection coherente de la tenue, le retour au modele de base, les objets
manquants, la liberation des vecteurs et le regroupement des clics rapides.

Les tests natifs utilisent un moteur simule : ils ne prouvent ni les pixels,
ni le temps de chargement des ressources du vrai jeu. La verification en partie
reste necessaire. Le statut Prepared indique que les appels natifs ont cree des
accessoires ; il ne mesure pas la fin du chargement du modele.

Diagnostic : `C:\Temp\opencode\wardrobe_appearance.log`.
