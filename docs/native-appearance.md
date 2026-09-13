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
preparation precedente. Une transformation survenue pendant le chargement
annule aussi la preparation. Apres dix secondes sans ressources disponibles,
l'ancienne tenue est conservee et une recuperation differee est programmee.
Un handle pret qui pointe encore vers `models/dev/error.vmdl` est refuse.

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
cas la tenue est re-appliquee par le chemin normal. Un modele temporaire
inconnu (dragon, hex, metamorphose...) est laisse au moteur : il ne declenche
pas une reconstruction du corps humain. Le retour au modele de base ou un
modele d'erreur declenche la recuperation. Les accessoires disparus sont
egalement detectes quand le heros est dans sa forme de base.

La limite de trois recuperations porte sur une serie d'echecs, plus sur toute
la duree d'une tenue. Trois secondes de rendu sain restaurent ce budget ;
des echecs persistants imposent une pause de trente secondes avant une autre
serie (`resyncs=` compte le total dans le journal). Les transformations
suivantes peuvent donc encore etre reparees sans changer d'equipement.

Les references des modeles de la tenue, y compris les formes alternatives,
restent detenues jusqu'au remplacement de la tenue ou au redemarrage de
l'entite. Une nouvelle creation native, une perte du heros local ou une
interruption de son ClientThink de plus de cinq secondes invalide les travaux
precedents. La reprise refait l'enregistrement et le chargement meme si le
pointeur, le handle et la selection sont identiques. Le modele de base connu
est conserve par identifiant de heros : un modele d'erreur a la reconnexion
ne devient pas le nouveau modele a restaurer.

Les vues, listes et references de ressources remplacees sont liberees par le
moteur. Le modele original est capture avant la construction des accessoires.
Le retour a une tenue classique restaure le modele visible ET la reference du
combineur ; conserver la reference de la persona melangeait deux squelettes.
Les objets du catalogue ont une position d'inventaire stable non nulle pour
eviter la notification de nouveaux objets a chaque reponse Equip.

## Profil binaire et mises a jour de Dota

Une mise a jour de Dota deplace le code bien plus souvent qu'elle ne le change.
Le profil (`src/native_appearance_profile.h`) n'est donc plus une liste d'adresses
de confiance : chaque adresse n'y est qu'un indice, et la DLL relocalise tout au
chargement (`src/native_appearance_resolver.h`).

- Une fonction est retrouvee par une signature d'octets masquee (immediats,
  deplacements rip et cibles d'appel effaces). L'indice est teste d'abord ; sinon
  la signature doit apparaitre exactement une fois dans `.text`.
- Les quelques fonctions sans signature unique (trois) sont retrouvees par
  l'instruction d'appel qui les atteint depuis une fonction deja localisee.
- Pour les 22 fonctions qui ont les deux, l'appel ne sert que de recoupement :
  il peut confirmer l'adresse de la signature, jamais la remplacer. Un desaccord
  arrete la resolution au lieu de laisser gagner l'ancre la plus faible.
- Chaque global, decalage de structure et slot de vtable est relu depuis
  l'instruction qui le porte, reperee par une fenetre d'octets masquee dans sa
  fonction. `JustInTimeManifest` est verifie en plus : l'adresse obtenue doit
  encore pointer sur ce litteral exact.
- Les decalages de schema (`m_steamID`, `m_hAssignedHero`) sont relus depuis la
  declaration situee a cote du nom du champ, qui survit aux deplacements.
- Aucun slot de vtable n'est deduit d'un autre : celui du lecteur de nom de
  modele vient de l'appel que le moteur fait lui-meme, pas de `FindOrLoad + 1`.
- Les tailles que le moteur remplit sont lues dans l'image quand elles y sont
  (`ModifierStorage`) ; le tampon de pile d'un `CEntityKeyValues`, dont la taille
  n'est encodee nulle part, est surdimensionne plutot que devine au plus juste.

Rien n'est ecrit dans le jeu pendant cette phase. Une entree ambigue ou
introuvable annule l'integration et se nomme dans le HUD (INSERT) et le journal,
au lieu de laisser un hook s'installer sur la mauvaise fonction. Le SHA-256 du
fichier ne conditionne plus rien : il indique seulement si c'est exactement la
build verifiee (`Profil client.dll` dans le HUD).

Installation de reference : `game/dota/bin/win64/client.dll`, SHA-256
`32785788b501bf7e7f8062c6d604c9daf8647f040dc6a18980535c691816efdd`, timestamp PE
`0x6aa07796`, SizeOfImage `0x6da1000`. Les adresses exactes vivent dans
`data/native_profile.json` et dans l'en-tete genere ; elles ne sont plus
maintenues a la main.

`refresh_profile.py` refait la meme resolution hors ligne, avec des ancres de
chaines et un parcours du graphe d'appels, et regenere l'en-tete. A lancer
seulement si la DLL signale une entree non resolue :

    python refresh_profile.py --check           # rien a faire ? l'annonce
    python refresh_profile.py                  # regenere le profil, puis build.bat
    python refresh_profile.py --accept-changes # idem, en enregistrant ce qui a bouge

Chaque valeur qui a change est listee avec son ancienne valeur, sa nouvelle et sa
nature. Un global est une adresse dans l'image : il bouge a chaque build, et une
mise a jour de Dota les deplace tous ensemble. Un decalage de structure ou un slot
de vtable est un fait d'ABI : une nouvelle valeur la est soit un vrai changement de
Dota, soit une ancre mal choisie, et le second cas ne doit pas partir en silence.

`--check` n'ecrit rien, donc une valeur qui a bouge y est un constat et non une
erreur ; seul le cas ABI fait echouer la verification, et donc l'avertissement de
`launch_level3.bat`. Le generateur, lui, refuse d'enregistrer quoi que ce soit qui
a change tant que `--accept-changes` n'est pas passe. Depuis la fenetre Wardrobe,
le Journal propose « Enregistrer quand meme » apres cet arret, une fois les lignes
lues.

## Validation

`tests/run_tests.bat` couvre le protocole, les reponses du recepteur, la restauration
des selections, la separation des comptes, les nouvelles creations de heros,
la selection coherente de la tenue, le retour au modele de base, les objets
manquants, la liberation des vecteurs et le regroupement des clics rapides.
Les regressions simulent aussi huit cycles dragon/humain, une forme inconnue,
une transformation pendant le chargement, plusieurs identifiants de heros,
la reconnexion avec le meme handle ou un nouveau, la perte de proprietaire,
la disparition du manifest, les erreurs de ressources et leur recuperation.

`tests\resolver_against_client.cpp` (lance par `run_tests.bat`) rejoue la resolution
sur le client.dll reellement installe, une fois telle quelle et une fois apres
avoir efface toutes les adresses enregistrees, et exige que chaque entree
retombe sur la valeur du profil.

Les tests natifs utilisent un moteur simule : ils ne prouvent ni les pixels,
ni le temps de chargement des ressources du vrai jeu. La verification en partie
reste necessaire. Le statut Prepared indique que les appels natifs ont cree des
accessoires ; il ne mesure pas la fin du chargement du modele.

Diagnostic : `C:\Temp\opencode\wardrobe_appearance.log`.
