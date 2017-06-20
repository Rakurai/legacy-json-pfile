/***************************************************************************
 *  Original Diku Mud copyright (C) 1990, 1991 by Sebastian Hammer,        *
 *  Michael Seifert, Hans Henrik St{rfeldt, Tom Madsen, and Katja Nyboe.   *
 *                                                                         *
 *  Merc Diku Mud improvments copyright (C) 1992, 1993 by Michael          *
 *  Chastain, Michael Quan, and Mitchell Tse.                              *
 *                                                                         *
 *  In order to use any part of this Merc Diku Mud, you must comply with   *
 *  both the original Diku license in 'license.doc' as well the Merc       *
 *  license in 'license.txt'.  In particular, you may not remove either of *
 *  these copyright notices.                                               *
 *                                                                         *
 *  Much time and thought has gone into this software and you are          *
 *  benefitting.  We hope that you share your changes too.  What goes      *
 *  around, comes around.                                                  *
 ***************************************************************************/

/***************************************************************************
*       ROM 2.4 is copyright 1993-1995 Russ Taylor                         *
*       ROM has been brought to you by the ROM consortium                  *
*           Russ Taylor (rtaylor@pacinfo.com)                              *
*           Gabrielle Taylor (gtaylor@pacinfo.com)                         *
*           Brian Moore (rom@rom.efn.org)                                  *
*       By using this code, you have agreed to follow the terms of the     *
*       ROM license, in the file Rom24/doc/rom.license                     *
***************************************************************************/

#include "merc.h"
#include "recycle.h"
#include "tables.h"
#include "lookup.h"
#include "deps/cJSON/cJSON.h"

extern  int     _filbuf         args((FILE *));
extern void     goto_line       args((CHAR_DATA *ch, int row, int column));
extern void     set_window      args((CHAR_DATA *ch, int top, int bottom));

#define CURRENT_VERSION         15   /* version number for pfiles */

bool debug_json = FALSE;

/* Locals */

int rename(const char *oldfname, const char *newfname);

const char *print_flags(int flag)
{
	int count, pos = 0;
	static char buf[52];

	for (count = 0; count < 32; count++) {
		if (IS_SET(flag, 1 << count)) {
			if (count < 26)
				buf[pos] = 'A' + count;
			else
				buf[pos] = 'a' + (count - 26);

			pos++;
		}
	}

	if (pos == 0) {
		buf[pos] = '0';
		pos++;
	}

	buf[pos] = '\0';
	return buf;
}

long read_flags(const char *str) {
	const char *p = str;
	long number = 0;
	bool sign = FALSE;

	if (*p == '-') {
		sign = TRUE;
		p++;
	}

	if (!isdigit(*p)) {
		while (('A' <= *p && *p <= 'Z') || ('a' <= *p && *p <= 'z')) {
			number += flag_convert(*p);
			p++;
		}
	}

	while (isdigit(*p)) {
		number = number * 10 + *p - '0';
		p++;
	}

	if (sign)
		number = 0 - number;

	if (*p == '|')
		number += read_flags(p+1);

	return number;
}

/*
 * Local functions.
 */
cJSON * fwrite_player     args((CHAR_DATA *ch));
cJSON * fwrite_char     args((CHAR_DATA *ch));
cJSON * fwrite_objects  args((CHAR_DATA *ch,  OBJ_DATA *head, bool strongbox));
cJSON * fwrite_pet      args((CHAR_DATA *pet));
void    fread_char      args((CHAR_DATA *ch,  cJSON *json, int version));
void    fread_player      args((CHAR_DATA *ch,  cJSON *json, int version));
void    fread_pet       args((CHAR_DATA *ch,  cJSON *json, int version));
void	fread_objects	args((CHAR_DATA *ch, cJSON *json, void (*obj_to)(OBJ_DATA *, CHAR_DATA *), int version));

void get_JSON_boolean(cJSON *obj, bool *target, const char *key);
void get_JSON_short(cJSON *obj, sh_int *target, const char *key);
void get_JSON_int(cJSON *obj, int *target, const char *key);
void get_JSON_long(cJSON *obj, long *target, const char *key);
void get_JSON_flags(cJSON *obj, long *target, const char *key);
void get_JSON_string(cJSON *obj, char **target, const char *key);

/*
 * Save a character and inventory.
 * Would be cool to save NPC's too for quest purposes,
 *   some of the infrastructure is provided.
 */
void save_char_obj(CHAR_DATA *ch)
{
	char strsave[MIL], buf[MSL];
	FILE *fp;

	if (ch == NULL || IS_NPC(ch))
		return;

	if (ch->desc != NULL && ch->desc->original != NULL)
		ch = ch->desc->original;

	ch->pcdata->last_saved = current_time;

	cJSON *root = cJSON_CreateObject();

	cJSON_AddNumberToObject(root, "version", CURRENT_VERSION);
	cJSON_AddItemToObject(root, "player", fwrite_player(ch));
	cJSON_AddItemToObject(root, "character", fwrite_char(ch));

	cJSON_AddItemToObject(root, "inventory", fwrite_objects(ch, ch->carrying, FALSE));
	cJSON_AddItemToObject(root, "locker", fwrite_objects(ch, ch->pcdata->locker, FALSE));
	cJSON_AddItemToObject(root, "strongbox", fwrite_objects(ch, ch->pcdata->strongbox, TRUE));

	if (ch->pet) {
		cJSON_AddItemToObject(root, "pet", fwrite_pet(ch->pet));
		cJSON_AddItemToObject(root, "pet_inventory", fwrite_objects(ch, ch->pet->carrying, FALSE));
	}

	char *JSONstring = cJSON_Print(root);
	cJSON_Delete(root);

	// added if to avoid closing invalid file
	one_argument(ch->name, buf);
	sprintf(strsave, "%s%s", PLAYER_DIR, capitalize(buf));

	if ((fp = fopen(TEMP_FILE, "w")) != NULL) {
		fputs(JSONstring, fp);
		fclose(fp);
		rename(TEMP_FILE, strsave);
	}
	else {
		bug("Save_char_obj: fopen", 0);
		perror(strsave);
	}

	update_pc_index(ch, FALSE);
}

void backup_char_obj(CHAR_DATA *ch)
{
	save_char_obj(ch);

	char strsave[MIL], strback[MIL], buf[MIL];
	one_argument(ch->name, buf);

	sprintf(strsave, "%s%s", PLAYER_DIR, capitalize(buf));
	sprintf(strback, "%s%s", BACKUP_DIR, capitalize(buf));

	sprintf(buf, "cp %s %s", strsave, strback);
	system(buf);
	sprintf(buf, "gzip -fq %s", strback);
	system(buf);
} /* end backup_char_obj() */

cJSON *fwrite_player(CHAR_DATA *ch)
{
	cJSON *item;
	cJSON *o = cJSON_CreateObject(); // object to return

	item = NULL;
	for (int pos = 0; pos < MAX_ALIAS; pos++) {
		if (!ch->pcdata->alias[pos][0]
		 || !ch->pcdata->alias_sub[pos][0])
			break;

		if (item == NULL)
			item = cJSON_CreateArray();

		cJSON *alias = cJSON_CreateArray();
		cJSON_AddItemToArray(alias, cJSON_CreateString(ch->pcdata->alias[pos]));
		cJSON_AddItemToArray(alias, cJSON_CreateString(ch->pcdata->alias_sub[pos]));
		cJSON_AddItemToArray(item, alias);
	}
	if (item != NULL)
		cJSON_AddItemToObject(o,	"Alias",		item);

	if (ch->pcdata->afk[0] != '\0')
		cJSON_AddStringToObject(o,	"Afk",			ch->pcdata->afk);

	cJSON_AddNumberToObject(o,		"Akills",		ch->pcdata->arenakills);
	cJSON_AddNumberToObject(o,		"Akilled",		ch->pcdata->arenakilled);

	if (ch->pcdata->aura[0])
		cJSON_AddStringToObject(o,	"Aura",			ch->pcdata->aura);

	cJSON_AddNumberToObject(o,		"Back",			ch->pcdata->backup);

	if (ch->pcdata->bamfin[0] != '\0')
		cJSON_AddStringToObject(o,	"Bin",			ch->pcdata->bamfin);

	if (ch->pcdata->bamfout[0] != '\0')
		cJSON_AddStringToObject(o,	"Bout",			ch->pcdata->bamfout);

	cJSON_AddStringToObject(o,		"Cgrp",			print_flags(ch->pcdata->cgroup));

	item = cJSON_CreateObject();
	cJSON_AddNumberToObject(item,	"drunk",		ch->pcdata->condition[COND_DRUNK]);
	cJSON_AddNumberToObject(item,	"full",			ch->pcdata->condition[COND_FULL]);
	cJSON_AddNumberToObject(item,	"thirst",		ch->pcdata->condition[COND_THIRST]);
	cJSON_AddNumberToObject(item,	"hunger",		ch->pcdata->condition[COND_HUNGER]);
	cJSON_AddItemToObject(o, 		"Cnd",	 		item);

	item = cJSON_CreateArray();
	for (int pos = 0; pos < MAX_COLORS; pos++) {
		if (ch->pcdata->color[pos] <= 0)
			continue;

		cJSON *p = cJSON_CreateObject();
		cJSON_AddNumberToObject(p, "slot", pos);
		cJSON_AddNumberToObject(p, "color", ch->pcdata->color[pos]);
		cJSON_AddNumberToObject(p, "bold", ch->pcdata->bold[pos]);
		cJSON_AddItemToArray(item, p);
	}
	cJSON_AddItemToObject(o,		"Colr",			item);

	if (ch->pcdata->deity[0])
		cJSON_AddStringToObject(o,	"Deit",			ch->pcdata->deity);

	if (ch->pcdata->email[0] != '\0')
		cJSON_AddStringToObject(o,	"Email",		ch->pcdata->email);

	cJSON_AddNumberToObject(o,		"Familiar",		ch->pcdata->familiar);

	if (ch->pcdata->fingerinfo[0] != '\0')
		cJSON_AddStringToObject(o,	"Finf",			ch->pcdata->fingerinfo);

	if (ch->pcdata->flag_killer)
		cJSON_AddNumberToObject(o,	"FlagKiller",	ch->pcdata->flag_killer);

	if (ch->pcdata->flag_thief)
		cJSON_AddNumberToObject(o,	"FlagThief",	ch->pcdata->flag_thief);

	if (ch->pcdata->gamein[0])
		cJSON_AddStringToObject(o,	"GameIn",		ch->pcdata->gamein);

	if (ch->pcdata->gameout[0])
		cJSON_AddStringToObject(o,	"GameOut",		ch->pcdata->gameout);

	item = NULL;
	for (int gn = 0; gn < MAX_GROUP; gn++) {
		if (group_table[gn].name == NULL || ch->pcdata->group_known[gn] == 0)
			continue;

		if (item == NULL)
			item = cJSON_CreateArray();

		cJSON_AddItemToArray(item, cJSON_CreateString(group_table[gn].name));
	}
	if (item != NULL)
		cJSON_AddItemToObject(o,	"Gr",			item);

	item = NULL;
	for (int pos = 0; pos < MAX_GRANT; pos++) {
		if (!ch->pcdata->granted_commands[pos][0])
			continue;

		if (item == NULL)
			item = cJSON_CreateArray();

		cJSON_AddItemToArray(item, cJSON_CreateString(ch->pcdata->granted_commands[pos]));
	}
	if (item != NULL)
		cJSON_AddItemToObject(o,	"Grant",		item);

	item = cJSON_CreateObject();
	cJSON_AddNumberToObject(item,	"hit",			ch->pcdata->perm_hit);
	cJSON_AddNumberToObject(item,	"mana",			ch->pcdata->perm_mana);
	cJSON_AddNumberToObject(item,	"stam",			ch->pcdata->perm_stam);
	cJSON_AddItemToObject(o, 		"HMSP",	 		item);

	item = NULL;
	for (int pos = 0; pos < MAX_IGNORE; pos++) {
		if (ch->pcdata->ignore[pos][0] == '\0')
			break;

		if (item == NULL)
			item = cJSON_CreateArray();

		cJSON_AddItemToArray(item, cJSON_CreateString(ch->pcdata->ignore[pos]));
	}
	if (item != NULL)
		cJSON_AddItemToObject(o,	"Ingore",		item);

	if (ch->pcdata->immname[0])
		cJSON_AddStringToObject(o,	"Immn",			ch->pcdata->immname);
	if (ch->pcdata->immprefix[0])
		cJSON_AddStringToObject(o,	"Immp",			ch->pcdata->immprefix);

	if (ch->class == PALADIN_CLASS) {
		cJSON_AddNumberToObject(o,	"Lay",			ch->pcdata->lays);
		cJSON_AddNumberToObject(o,	"Lay_Next",		ch->pcdata->next_lay_countdown);
	}

	cJSON_AddNumberToObject(o,		"LLev",			ch->pcdata->last_level);
	cJSON_AddNumberToObject(o,		"LogO",			current_time);

	if (ch->pcdata->last_lsite[0])
		cJSON_AddStringToObject(o,	"Lsit",			ch->pcdata->last_lsite);

	cJSON_AddStringToObject(o,		"Ltim",			dizzy_ctime(&ch->pcdata->last_ltime));
	cJSON_AddStringToObject(o,		"LSav",			dizzy_ctime(&ch->pcdata->last_saved));

	if (ch->pcdata->mark_room)
		cJSON_AddNumberToObject(o,	"Mark",			ch->pcdata->mark_room);

	cJSON_AddNumberToObject(o,		"Mexp",			ch->pcdata->mud_exp);

	item = cJSON_CreateObject();
	cJSON_AddNumberToObject(item,	"note",			ch->pcdata->last_note);
	cJSON_AddNumberToObject(item,	"idea",			ch->pcdata->last_idea);
	cJSON_AddNumberToObject(item,	"role",			ch->pcdata->last_roleplay);
	cJSON_AddNumberToObject(item,	"quest",		ch->pcdata->last_immquest);
	cJSON_AddNumberToObject(item,	"change",		ch->pcdata->last_changes);
	cJSON_AddNumberToObject(item,	"pers",			ch->pcdata->last_personal);
	cJSON_AddNumberToObject(item,	"trade",		ch->pcdata->last_trade);
	cJSON_AddItemToObject(o, 		"Note", 		item);

	cJSON_AddStringToObject(o,		"Pass",			ch->pcdata->pwd);
	cJSON_AddNumberToObject(o,		"PCkills",		ch->pcdata->pckills);
	cJSON_AddNumberToObject(o,		"PCkilled",		ch->pcdata->pckilled);
	cJSON_AddNumberToObject(o,		"PKRank",		ch->pcdata->pkrank);
	cJSON_AddStringToObject(o,		"Plr",			print_flags(ch->pcdata->plr));
	cJSON_AddNumberToObject(o,		"Plyd",			ch->pcdata->played);
	cJSON_AddNumberToObject(o,		"Pnts",			ch->pcdata->points);

	item = NULL;
	for (int pos = 0; pos < MAX_QUERY; pos++) {
		if (ch->pcdata->query[pos][0] == '\0')
			break;

		if (item == NULL)
			item = cJSON_CreateArray();

		cJSON_AddItemToArray(item, cJSON_CreateString(ch->pcdata->query[pos]));
	}
	if (item != NULL)
		cJSON_AddItemToObject(o,	"Query",		item);

	if (ch->pcdata->rank[0])
		cJSON_AddStringToObject(o,	"Rank",			ch->pcdata->rank);

	if (ch->pcdata->rolepoints)
		cJSON_AddNumberToObject(o,	"RolePnts",		ch->pcdata->rolepoints);

	item = NULL;
	for (int sn = 0; sn < MAX_SKILL; sn++) {
		if (skill_table[sn].name == NULL)
			break;

		if (ch->pcdata->learned[sn] <= 0)
			continue;

		if (item == NULL)
			item = cJSON_CreateArray();

		if (ch->pcdata->evolution[sn] < 1)
			ch->pcdata->evolution[sn] = 1;
		else if (ch->pcdata->evolution[sn] > 4)
			ch->pcdata->evolution[sn] = 4;

		cJSON *sk = cJSON_CreateObject();
		cJSON_AddStringToObject(sk, "name", skill_table[sn].name);
		cJSON_AddNumberToObject(sk, "prac", ch->pcdata->learned[sn]);
		cJSON_AddNumberToObject(sk, "evol", ch->pcdata->evolution[sn]);
		cJSON_AddItemToArray(item, sk);
	}
	if (item != NULL)
		cJSON_AddItemToObject(o,	"Sk",			item);

	if (ch->pcdata->skillpoints)
		cJSON_AddNumberToObject(o,	"SkillPnts",	ch->pcdata->skillpoints);

	if (ch->pcdata->spouse[0])
		cJSON_AddStringToObject(o,	"Spou",			ch->pcdata->spouse);

	if (ch->pcdata->nextsquest)
		cJSON_AddNumberToObject(o,	"SQuestNext",	ch->pcdata->nextsquest);
	else if (ch->pcdata->sqcountdown)
		cJSON_AddNumberToObject(o,	"SQuestNext",	20);

	if (ch->pcdata->remort_count > 0) {
		if (ch->pcdata->status[0])
			cJSON_AddStringToObject(o,	"Stus",		ch->pcdata->status);

		cJSON_AddNumberToObject(o,	"RmCt",			ch->pcdata->remort_count);

		item = cJSON_CreateArray();

		for (int i = 0; i < (ch->pcdata->remort_count / 20) + 1; i++) {
			int slot = skill_table[ch->pcdata->extraclass[i]].slot;
			cJSON_AddItemToArray(item, cJSON_CreateNumber(slot));
		}

		cJSON_AddItemToObject(o, "ExSk", item);

		item = cJSON_CreateIntArray(ch->pcdata->raffect, ch->pcdata->remort_count / 10 + 1);
		cJSON_AddItemToObject(o, "Raff", item);
	}

	item = cJSON_CreateObject();
	cJSON_AddNumberToObject(item,	"hit",			ch->pcdata->trains_to_hit);
	cJSON_AddNumberToObject(item,	"mana",			ch->pcdata->trains_to_mana);
	cJSON_AddNumberToObject(item,	"stam",			ch->pcdata->trains_to_stam);
	cJSON_AddItemToObject(o, 		"THMS",	 		item);

	if (ch->pcdata->title[0])
		cJSON_AddStringToObject(o,	"Titl",			ch->pcdata->title[0] == ' ' ?
		ch->pcdata->title+1 : ch->pcdata->title);
	cJSON_AddNumberToObject(o,		"TSex",			ch->pcdata->true_sex);
	cJSON_AddStringToObject(o,		"Video",		print_flags(ch->pcdata->video));

	if (ch->pcdata->whisper[0])
		cJSON_AddStringToObject(o,	"Wspr",			ch->pcdata->whisper);

	return o;
}

/*
 * Write the char.
 */
cJSON *fwrite_char(CHAR_DATA *ch)
{
	cJSON *item;
	cJSON *o = cJSON_CreateObject(); // object to return

	cJSON_AddStringToObject(o,		"Act",			print_flags(ch->act));
	cJSON_AddStringToObject(o,		"AfBy",			print_flags(ch->affected_by));

	item = NULL;
	for (AFFECT_DATA *paf = ch->affected; paf != NULL; paf = paf->next) {
		if (paf->type < 0 || paf->type >= MAX_SKILL)
			continue;

		if (item == NULL)
			item = cJSON_CreateArray();

		cJSON *aff = cJSON_CreateObject();
		cJSON_AddStringToObject(aff, "name", skill_table[paf->type].name);
		cJSON_AddNumberToObject(aff, "where", paf->where);
		cJSON_AddNumberToObject(aff, "level", paf->level);
		cJSON_AddNumberToObject(aff, "dur", paf->duration);
		cJSON_AddNumberToObject(aff, "mod", paf->modifier);
		cJSON_AddNumberToObject(aff, "loc", paf->location);
		cJSON_AddNumberToObject(aff, "bitv", paf->bitvector);
		cJSON_AddNumberToObject(aff, "evo", paf->evolution);
		cJSON_AddItemToArray(item, aff);
	}
	if (item != NULL)
		cJSON_AddItemToObject(o,	"Affc",			item);

	cJSON_AddNumberToObject(o,		"Alig",			ch->alignment);

	item = cJSON_CreateObject();
	cJSON_AddNumberToObject(item,	"str",			ch->perm_stat[STAT_STR]);
	cJSON_AddNumberToObject(item,	"int",			ch->perm_stat[STAT_INT]);
	cJSON_AddNumberToObject(item,	"wis",			ch->perm_stat[STAT_WIS]);
	cJSON_AddNumberToObject(item,	"dex",			ch->perm_stat[STAT_DEX]);
	cJSON_AddNumberToObject(item,	"con",			ch->perm_stat[STAT_CON]);
	cJSON_AddNumberToObject(item,	"chr",			ch->perm_stat[STAT_CHR]);
	cJSON_AddItemToObject(o, 		"Atrib", 		item);


	if (ch->clan)
		cJSON_AddStringToObject(o,	"Clan",			ch->clan->name);

	cJSON_AddNumberToObject(o,		"Cla",			ch->class);
	cJSON_AddStringToObject(o,		"Cnsr",			print_flags(ch->censor));
	cJSON_AddStringToObject(o,		"Comm",			print_flags(ch->comm));

	if (ch->description[0])
		cJSON_AddStringToObject(o,	"Desc",			ch->description);

	cJSON_AddNumberToObject(o,		"Exp",			ch->exp);
	cJSON_AddStringToObject(o,		"FImm",			print_flags(ch->imm_flags));
	cJSON_AddStringToObject(o,		"FRes",			print_flags(ch->res_flags));
	cJSON_AddStringToObject(o,		"FVul",			print_flags(ch->vuln_flags));

	if (ch->gold_donated)
		cJSON_AddNumberToObject(o,	"GlDonated",	ch->gold_donated);

	cJSON_AddNumberToObject(o,		"Gold",			ch->gold);

	if (ch->gold_in_bank > 0)
		cJSON_AddNumberToObject(o,	"Gold_in_bank",	ch->gold_in_bank);

	item = cJSON_CreateObject();
	cJSON_AddNumberToObject(item,	"hit",			ch->hit);
	cJSON_AddNumberToObject(item,	"mana",			ch->mana);
	cJSON_AddNumberToObject(item,	"stam",			ch->stam);
	cJSON_AddItemToObject(o, 		"HMS",	 		item);

	cJSON_AddNumberToObject(o,		"Id",			ch->id);
	cJSON_AddNumberToObject(o,		"Levl",			ch->level);

	if (ch->long_descr[0])
		cJSON_AddStringToObject(o,	"LnD",			ch->long_descr);

	cJSON_AddStringToObject(o,		"Name",			ch->name);
	cJSON_AddNumberToObject(o,		"Pos",			ch->position);
	cJSON_AddNumberToObject(o,		"Prac",			ch->practice);

	if (ch->prompt[0])
		cJSON_AddStringToObject(o,	"Prom",			ch->prompt);

	if (ch->questpoints_donated)
		cJSON_AddNumberToObject(o,	"QpDonated",	ch->questpoints_donated);

	if (ch->questpoints)
		cJSON_AddNumberToObject(o,	"QuestPnts",	ch->questpoints);

	if (ch->nextquest)
		cJSON_AddNumberToObject(o,	"QuestNext",	ch->nextquest);
	else if (ch->countdown)
		cJSON_AddNumberToObject(o,	"QuestNext",	12);

	cJSON_AddStringToObject(o,		"Race",			pc_race_table[ch->race].name);
	cJSON_AddStringToObject(o,		"Revk",			print_flags(ch->revoke));
	cJSON_AddNumberToObject(o,		"Room",			
		(ch->in_room == get_room_index(ROOM_VNUM_LIMBO) && ch->was_in_room != NULL)
	        ? ch->was_in_room->vnum
	        : ch->in_room == NULL
	        ? 3001
	        : ch->in_room->vnum);
	cJSON_AddNumberToObject(o,		"Scro",			ch->level);
	cJSON_AddNumberToObject(o,		"Silv",			ch->silver);

	if (ch->silver_in_bank > 0)
		cJSON_AddNumberToObject(o,	"Silver_in_bank", ch->silver_in_bank);

	if (ch->short_descr[0])
		cJSON_AddStringToObject(o,	"ShD",			ch->short_descr);

	cJSON_AddNumberToObject(o,		"Trai",			ch->train);
	cJSON_AddNumberToObject(o,		"Wimp",			ch->wimpy);

	if (IS_IMMORTAL(ch)) { // why aren't these pcdata?
		cJSON_AddStringToObject(o,	"Wizn",			print_flags(ch->wiznet));
		cJSON_AddNumberToObject(o,	"Invi",			ch->invis_level);
		cJSON_AddNumberToObject(o,	"Lurk",			ch->lurk_level);
		cJSON_AddNumberToObject(o,	"Secu",			ch->secure_level);
	}

	return o;
}

/* write a pet */
cJSON *fwrite_pet(CHAR_DATA *pet)
{
	cJSON *item;
	cJSON *o = fwrite_char(pet);

	cJSON_AddNumberToObject(o, "Vnum", pet->pIndexData->vnum);

	// reset_char sets this back to pIndexData on load, but whatever
	cJSON_AddNumberToObject(o, "Sex", pet->sex);

	if (pet->saving_throw != 0)
		cJSON_AddNumberToObject(o, "Save", pet->saving_throw);

	if (pet->hitroll != pet->pIndexData->hitroll)
		cJSON_AddNumberToObject(o, "Hit", pet->hitroll);

	if (pet->damroll != pet->pIndexData->damage[DICE_BONUS])
		cJSON_AddNumberToObject(o, "Dam", pet->damroll);

	item = cJSON_CreateObject();
	cJSON_AddNumberToObject(item, "str", pet->mod_stat[STAT_STR]);
	cJSON_AddNumberToObject(item, "int", pet->mod_stat[STAT_INT]);
	cJSON_AddNumberToObject(item, "wis", pet->mod_stat[STAT_WIS]);
	cJSON_AddNumberToObject(item, "dex", pet->mod_stat[STAT_DEX]);
	cJSON_AddNumberToObject(item, "con", pet->mod_stat[STAT_CON]);
	cJSON_AddNumberToObject(item, "chr", pet->mod_stat[STAT_CHR]);
	cJSON_AddItemToObject(o, "AtMod", item);

	return o;
}

/*
 * Write an object and its contents.
 */
cJSON *fwrite_obj(CHAR_DATA *ch, OBJ_DATA *obj, bool strongbox)
{
	/*
	 * Castrate storage characters.
	 */
	if (!IS_IMMORTAL(ch))
		if ((!strongbox && (obj->level > get_holdable_level(ch)))
		    || (obj->item_type == ITEM_KEY && (obj->value[0] == 0))
		    || (obj->item_type == ITEM_MAP && !obj->value[0]))
			return NULL;

	cJSON *item;
	cJSON *o = cJSON_CreateObject();

	if (obj->condition != obj->pIndexData->condition)
		cJSON_AddNumberToObject(o,	"Cond",			obj->condition);
	if (obj->cost != obj->pIndexData->cost)
		cJSON_AddNumberToObject(o,	"Cost",			obj->cost);
	if (obj->description != obj->pIndexData->description)
		cJSON_AddStringToObject(o,	"Desc",			obj->description);
	if (obj->enchanted) {
//		cJSON_AddNumberToObject(o,	"Enchanted",	obj->enchanted);
		item = NULL;
		for (AFFECT_DATA *paf = obj->affected; paf != NULL; paf = paf->next) {
			if (paf->type < 0 || paf->type >= MAX_SKILL)
				continue;

			if (item == NULL)
				item = cJSON_CreateArray();

			cJSON *aff = cJSON_CreateObject();
			cJSON_AddStringToObject(aff, "name", skill_table[paf->type].name);
			cJSON_AddNumberToObject(aff, "where", paf->where);
			cJSON_AddNumberToObject(aff, "level", paf->level);
			cJSON_AddNumberToObject(aff, "dur", paf->duration);
			cJSON_AddNumberToObject(aff, "mod", paf->modifier);
			cJSON_AddNumberToObject(aff, "loc", paf->location);
			cJSON_AddNumberToObject(aff, "bitv", paf->bitvector);
			cJSON_AddNumberToObject(aff, "evo", paf->evolution);
			cJSON_AddItemToArray(item, aff);
		}
		if (item != NULL)
			cJSON_AddItemToObject(o,	"Affc",			item);
	}

	item = NULL;
	for (EXTRA_DESCR_DATA *ed = obj->extra_descr; ed != NULL; ed = ed->next) {
		if (item == NULL)
			item = cJSON_CreateObject();

		cJSON_AddStringToObject(item, ed->keyword, ed->description);
	}
	if (item != NULL)
		cJSON_AddItemToObject(o,	"ExDe",			item);

	if (obj->extra_flags != obj->pIndexData->extra_flags)
		cJSON_AddNumberToObject(o,	"ExtF",			obj->extra_flags);
	if (obj->item_type != obj->pIndexData->item_type)
		cJSON_AddNumberToObject(o,	"Ityp",			obj->item_type);
	if (obj->level != obj->pIndexData->level)
		cJSON_AddNumberToObject(o,	"Lev",			obj->level);
	if (obj->material != obj->pIndexData->material)
		cJSON_AddStringToObject(o,	"Mat",			obj->material);
	if (obj->name != obj->pIndexData->name)
		cJSON_AddStringToObject(o,	"Name",			obj->name);
	if (obj->short_descr != obj->pIndexData->short_descr)
		cJSON_AddStringToObject(o,	"ShD",			obj->short_descr);

	/*
	 * Spelled eq by Demonfire
	 * Added on 11.23.1996
	 */
	item = NULL;
	for (int pos = 0; pos < MAX_SPELL; pos++) {
		if (obj->spell[pos] == 0)
			continue;

		if (item == NULL)
			item = cJSON_CreateArray();

		cJSON *sp = cJSON_CreateObject();
		cJSON_AddStringToObject(sp, "name", skill_table[obj->spell[pos]].name);
		cJSON_AddNumberToObject(sp, "level", obj->spell_lev[pos]);
		cJSON_AddItemToArray(item, sp);
	}
	if (item != NULL)
		cJSON_AddItemToObject(o,	"Splxtra",		item);

	if (obj->timer != 0)
		cJSON_AddNumberToObject(o,	"Time",			obj->timer);

	if (obj->value[0] != obj->pIndexData->value[0]
	    ||  obj->value[1] != obj->pIndexData->value[1]
	    ||  obj->value[2] != obj->pIndexData->value[2]
	    ||  obj->value[3] != obj->pIndexData->value[3]
	    ||  obj->value[4] != obj->pIndexData->value[4])
	    cJSON_AddItemToObject(o,	"Val",			cJSON_CreateIntArray(obj->value, 5));

	cJSON_AddNumberToObject(o,		"Vnum",			obj->pIndexData->vnum);

	if (obj->wear_loc != WEAR_NONE)
		cJSON_AddNumberToObject(o,	"Wear",			obj->wear_loc);

	if (obj->wear_flags != obj->pIndexData->wear_flags)
		cJSON_AddNumberToObject(o,	"WeaF",			obj->wear_flags);
	if (obj->weight != obj->pIndexData->weight)
		cJSON_AddNumberToObject(o,	"Wt",			obj->weight);

	// does nothing if the contains is NULL
	cJSON_AddItemToObject(o, "contains", fwrite_objects(ch, obj->contains, strongbox));

	return o;
} /* end fwrite_obj() */

cJSON *fwrite_objects(CHAR_DATA *ch, OBJ_DATA *head, bool strongbox) {
	cJSON *array = cJSON_CreateArray();

	// old way was to use recursion to write items in reverse order, so loading would
	// be in the original order.  same concept here, except no recursion; we just
	// take advantage of the linked list underlying the cJSON array and insert at
	// index 0, so the array is written backwards.
	for (OBJ_DATA *obj = head; obj; obj = obj->next_content)
		cJSON_InsertItemInArray(array, 0, fwrite_obj(ch, obj, strongbox));

	// because objects could be nerfed on saving, this could still be empty
	if (cJSON_GetArraySize(array) == 0) {
		cJSON_Delete(array);
		array = NULL;
	}

	return array;
}

/*
 * Load a char and inventory into a new ch structure.
 */
bool load_char_obj(DESCRIPTOR_DATA *d, const char *name)
{
	char strsave[MAX_INPUT_LENGTH];
	CHAR_DATA *ch;
	FILE *fp;
	bool found;
	int stat;
	ch = new_char();
	ch->pcdata = new_pcdata();
	d->character                        = ch;
	ch->desc                            = d;
	ch->name                            = str_dup(name);
	ch->id                              = get_pc_id();
	ch->race                            = race_lookup("human");
	ch->act                             = PLR_NOSUMMON | PLR_AUTOASSIST | PLR_AUTOEXIT | PLR_AUTOLOOT |
	                                      PLR_AUTOSAC | PLR_AUTOSPLIT | PLR_AUTOGOLD | PLR_TICKS | PLR_WIMPY |
	                                      PLR_COLOR | PLR_COLOR2;
	ch->comm                            = COMM_COMBINE | COMM_PROMPT;
	ch->secure_level                    = RANK_IMM;
	ch->censor                          = CENSOR_CHAN;    /* default rating is PG */
	ch->prompt                          = str_dup("%CW<%CC%h%CThp %CG%m%CHma %CB%v%CNst%CW> ");
	ch->pcdata->ch                      = ch;
	ch->pcdata->deity                   = str_dup("Nobody");
	ch->pcdata->mud_exp                 = MEXP_LEGACY_OLDBIE;
	ch->pcdata->plr                     = PLR_NEWSCORE;

	for (stat = 0; stat < MAX_STATS; stat++)
		ch->perm_stat[stat]             = 3;

	ch->pcdata->condition[COND_THIRST]  = 48;
	ch->pcdata->condition[COND_FULL]    = 48;
	ch->pcdata->condition[COND_HUNGER]  = 48;
	ch->pcdata->perm_hit            = 20;
	ch->pcdata->perm_mana           = 100;
	ch->pcdata->perm_stam           = 100;
	ch->pcdata->last_logoff         = current_time;
	found = FALSE;
	// added if here
	/* decompress if .gz file exists */
	/*    #if defined(unix)
	    sprintf( strsave, "%s%s%s", PLAYER_DIR, capitalize(name),".gz");
	    if ( ( fp = fopen( strsave, "r" ) ) != NULL )
	    {
	        fclose(fp);
	        sprintf(buf,"gzip -dfq %s",strsave);
	        system(buf);
	    }
	    #endif */
	sprintf(strsave, "%s%s", PLAYER_DIR, capitalize(name));

	cJSON *root = NULL;

	if ((fp = fopen(strsave, "rb")) != NULL) {
		int length;
		char *buffer;

		fseek (fp, 0, SEEK_END);
		length = ftell (fp);
		fseek (fp, 0, SEEK_SET);
		buffer = malloc (length);

		fread (buffer, 1, length, fp);
		fclose (fp);

		root = cJSON_Parse(buffer);
		free(buffer);
	}

	if (root != NULL) {

		int version = CURRENT_VERSION;
		get_JSON_int(root, &version, "version");

		fread_char(ch, cJSON_GetObjectItem(root, "character"), version);
		fread_player(ch, cJSON_GetObjectItem(root, "player"), version);

		fread_objects(ch, cJSON_GetObjectItem(root, "inventory"), &obj_to_char, version);
		fread_objects(ch, cJSON_GetObjectItem(root, "locker"), &obj_to_locker, version);
		fread_objects(ch, cJSON_GetObjectItem(root, "strongbox"), &obj_to_strongbox, version);

		fread_pet(ch, cJSON_GetObjectItem(root, "pet"), version);

		if (ch->pet)
			fread_objects(ch->pet, cJSON_GetObjectItem(root, "pet_inventory"), &obj_to_char, version);

		cJSON_Delete(root); // finished with it
		found = TRUE;

		// fix things up

		// fix up character stuff here
		if (ch->in_room == NULL)
			ch->in_room = get_room_index(ROOM_VNUM_LIMBO);

		if (ch->secure_level > GET_RANK(ch))
			ch->secure_level = GET_RANK(ch);

		/* removed holylight at 12 -- Montrey */
		if (version < 12 && IS_SET(ch->act, N))
			REMOVE_BIT(ch->act, N);

		// switching to cgroups with old pfiles -- Montrey (2014)
		if (version < 15 && IS_SET(ch->act, N)) { // deputy
			REMOVE_BIT(ch->act, N);
			SET_CGROUP(ch, GROUP_DEPUTY);
		}

		if (version < 15 && IS_SET(ch->act, ee)) { // leader
			REMOVE_BIT(ch->act, ee);
			SET_CGROUP(ch, GROUP_LEADER);
		}

		if (ch->pcdata->remort_count > 0) {
			SET_CGROUP(ch, GROUP_AVATAR);
			SET_CGROUP(ch, GROUP_HERO);
		}

		if (ch->level >= LEVEL_AVATAR)
			SET_CGROUP(ch, GROUP_AVATAR);

		if (ch->level >= LEVEL_HERO)
			SET_CGROUP(ch, GROUP_HERO);

		if (ch->clan == NULL && !IS_IMMORTAL(ch)) {
			REM_CGROUP(ch, GROUP_LEADER);
			REM_CGROUP(ch, GROUP_DEPUTY);
		}

		if (ch->clan != NULL)
			SET_CGROUP(ch, GROUP_CLAN);

		if (!IS_IMMORTAL(ch)) {
			for (int stat = 0; stat < (MAX_STATS); stat++) {
				/* make sure stats aren't above race max, for possible changes to race maximums */
				if (stat == class_table[ch->class].attr_prime) {
					if (ch->race == 1) { /* humans */
						if (ch->perm_stat[stat] > (pc_race_table[ch->race].max_stats[stat] + 3))
							ch->perm_stat[stat] = (pc_race_table[ch->race].max_stats[stat] + 3);
					}
					else {
						if (ch->perm_stat[stat] > (pc_race_table[ch->race].max_stats[stat] + 2))
							ch->perm_stat[stat] = (pc_race_table[ch->race].max_stats[stat] + 2);
					}
				}
				else if (ch->perm_stat[stat] > pc_race_table[ch->race].max_stats[stat])
					ch->perm_stat[stat] = pc_race_table[ch->race].max_stats[stat];
			}
		}
	}

	/* initialize race */
	if (found) {
		int i, percent;

		if (ch->race == 0)
			ch->race = race_lookup("human");

		ch->size = pc_race_table[ch->race].size;
		ch->dam_type = 17; /*punch */

		for (i = 0; i < 5; i++) {
			if (pc_race_table[ch->race].skills[i] == NULL)
				break;

			group_add(ch, pc_race_table[ch->race].skills[i], FALSE);
		}

		ch->affected_by = ch->affected_by | race_table[ch->race].aff;
		ch->imm_flags   = ch->imm_flags | race_table[ch->race].imm;
		ch->res_flags   = ch->res_flags | race_table[ch->race].res;
		ch->vuln_flags  = ch->vuln_flags | race_table[ch->race].vuln;
		ch->form        = race_table[ch->race].form;
		ch->parts       = race_table[ch->race].parts;

		/* let's make sure their remort affect vuln/res is ok */
		for (i = 0; ch->pcdata->remort_count && i <= ch->pcdata->remort_count / 10 + 1; i++) {
			if (ch->pcdata->raffect[i] >= 900 && ch->pcdata->raffect[i] <= 949)
				SET_BIT(ch->vuln_flags, raffects[raff_lookup(ch->pcdata->raffect[i])].add);
			else if (ch->pcdata->raffect[i] >= 950 && ch->pcdata->raffect[i] <= 999)
				SET_BIT(ch->res_flags, raffects[raff_lookup(ch->pcdata->raffect[i])].add);
		}

		/* fix command groups */
		REMOVE_BIT(ch->act, (ee));      /* PLR_LEADER */
		REMOVE_BIT(ch->act, (N));       /* PLR_DEPUTY */
		SET_CGROUP(ch, GROUP_PLAYER);

		/* nuke wiznet flags beyond their level, in case they were temp trusted */
		if (ch->wiznet)
			for (i = 0; wiznet_table[i].name != NULL; i++)
				if (IS_SET(ch->wiznet, wiznet_table[i].flag) && GET_RANK(ch) < wiznet_table[i].level)
					REMOVE_BIT(ch->wiznet, wiznet_table[i].flag);

		reset_char(ch);
		/* adjust hp mana stamina up  -- here for speed's sake */
		percent = (current_time - ch->pcdata->last_logoff) * 25 / (2 * 60 * 60);
		percent = UMIN(percent, 100);

		if (percent > 0 && !IS_AFFECTED(ch, AFF_POISON) && !IS_AFFECTED(ch, AFF_PLAGUE)) {
			ch->hit         += (ch->max_hit - ch->hit) * percent / 100;
			ch->mana        += (ch->max_mana - ch->mana) * percent / 100;
			ch->stam        += (ch->max_stam - ch->stam) * percent / 100;
		}
	}

	return found;
}

/*
 * Read in a char.
 */

#if defined(STRKEY)
#undef STRKEY
#endif

#define STRKEY( literal, field, value )                                    \
	if ( !str_cmp( key, literal ) )        \
	{                                       \
		free_string(field);					\
		field = str_dup(value);	\
		fMatch = TRUE;						\
		break;                              \
	}


#if defined(INTKEY)
#undef INTKEY
#endif

#define INTKEY( literal, field, value )                                    \
	if ( !str_cmp( key, literal ) )        \
	{                                       \
		field  = value;               \
		fMatch = TRUE;                      \
		break;                              \
	}

#if defined(SKIPKEY)
#undef SKIPKEY
#endif

#define SKIPKEY( literal )                  \
	if ( !str_cmp( key, literal ) )			\
	{                                       \
		fMatch = TRUE;                      \
		break;                              \
	}	


void get_JSON_boolean(cJSON *obj, bool *target, const char *key) {
	cJSON *val = cJSON_GetObjectItem(obj, key);

	if (val != NULL)
		*target = (val->valueint != 0);
}

void get_JSON_short(cJSON *obj, sh_int *target, const char *key) {
	cJSON *val = cJSON_GetObjectItem(obj, key);

	if (val != NULL)
		*target = val->valueint;
}

void get_JSON_int(cJSON *obj, int *target, const char *key) {
	cJSON *val = cJSON_GetObjectItem(obj, key);

	if (val != NULL)
		*target = val->valueint;
}

void get_JSON_long(cJSON *obj, long *target, const char *key) {
	cJSON *val = cJSON_GetObjectItem(obj, key);

	if (val != NULL)
		*target = val->valueint;
}

void get_JSON_flags(cJSON *obj, long *target, const char *key) {
	cJSON *val = cJSON_GetObjectItem(obj, key);

	if (val != NULL)
		*target = read_flags(val->valuestring);
}

void get_JSON_string(cJSON *obj, char **target, const char *key) {
	cJSON *val = cJSON_GetObjectItem(obj, key);

	if (val != NULL) {
		if (*target != NULL) {
			free_string(*target);
		}
		*target = str_dup(val->valuestring);
	}
}

void fread_player(CHAR_DATA *ch, cJSON *json, int version) {
	if (json == NULL)
		return;

	// if there are any player-specific fields that are depended on by others in the list,
	// load them right here, and make sure to use SKIPKEY(key) in the switch

	// none


	for (cJSON *o = json->child; o; o = o->next) {
		char *key = o->string;
		bool fMatch = FALSE;
		int count = 0; // convenience variable to compact this list, resets with every item

		switch (toupper(key[0])) {
			case 'A':
				if (!str_cmp(key, "Alias")) { // array of 2-tuples
					// each alias is a 2-tuple (a list)
					for (cJSON *item = o->child; item != NULL; item = item->next, count++) {
						ch->pcdata->alias[count] = str_dup(item->child->valuestring);
						ch->pcdata->alias_sub[count] = str_dup(item->child->next->valuestring);
					}
					fMatch = TRUE; break;
				}

				STRKEY("Afk",			ch->pcdata->afk,			o->valuestring);
				INTKEY("Akills",		ch->pcdata->arenakills,		o->valueint);
				INTKEY("Akilled",		ch->pcdata->arenakilled, 	o->valueint);
				STRKEY("Aura",			ch->pcdata->aura,			o->valuestring);
				break;
			case 'B':
				INTKEY("Back",			ch->pcdata->backup,			o->valueint);
				STRKEY("Bin",			ch->pcdata->bamfin,			o->valuestring);
				STRKEY("Bout",			ch->pcdata->bamfout,		o->valuestring);
				break;
			case 'C':
				if (!str_cmp(key, "Cnd")) { // 4-tuple
					get_JSON_short(o, &ch->pcdata->condition[COND_DRUNK], "drunk");
					get_JSON_short(o, &ch->pcdata->condition[COND_FULL], "full");
					get_JSON_short(o, &ch->pcdata->condition[COND_THIRST], "thirst");
					get_JSON_short(o, &ch->pcdata->condition[COND_HUNGER], "hunger");
					fMatch = TRUE; break;
				}

				if (!str_cmp(key, "Colr")) { // array of dicts
					for (cJSON *item = o->child; item != NULL; item = item->next) {
						int slot = cJSON_GetObjectItem(item, "slot")->valueint;
						get_JSON_short(item, &ch->pcdata->color[slot], "color");
						get_JSON_short(item, &ch->pcdata->bold[slot], "bold");
					}
					fMatch = TRUE; break;
				}

				INTKEY("Cgrp",			ch->pcdata->cgroup,			read_flags(o->valuestring));
				break;
			case 'D':
				STRKEY("Deit",			ch->pcdata->deity,			o->valuestring);
				break;
			case 'E':
				if (!str_cmp(key, "ExSk")) {
					for (cJSON *item = o->child; item != NULL && count < MAX_EXTRACLASS_SLOTS; item = item->next) {
						bool found = FALSE;

						for (int i = 1; i < MAX_SKILL; i++) {
							if (skill_table[i].slot == item->valueint) {
								ch->pcdata->extraclass[count++] = i;
								found = TRUE;
								break;
							}
						}

						if (!found) {
							bug("Unknown extraclass skill.", 0);
						}
					}
					fMatch = TRUE; break;
				}

				STRKEY("Email",			ch->pcdata->email,			o->valuestring);
				break;
			case 'F':
				INTKEY("Familiar",		ch->pcdata->familiar,		o->valueint);
				STRKEY("Finf",			ch->pcdata->fingerinfo,		o->valuestring);
				INTKEY("FlagThief",		ch->pcdata->flag_thief,		o->valueint);
				INTKEY("FlagKiller",	ch->pcdata->flag_killer,	o->valueint);
				break;
			case 'G':
				if (!str_cmp(key, "Grant")) {
					for (cJSON *item = o->child; item != NULL && count < MAX_GRANT; item = item->next)
						strcpy(ch->pcdata->granted_commands[count++], item->valuestring);
					fMatch = TRUE; break;
				}

				if (!str_cmp(key, "Gr")) {
					for (cJSON *item = o->child; item != NULL; item = item->next) {
						int gn = group_lookup(item->valuestring);

						if (gn < 0) {
							fprintf(stderr, "%s", item->valuestring);
							bug("Unknown group. ", 0);
							continue;
						}

						gn_add(ch, gn);
					}
					fMatch = TRUE; break;
				}

				STRKEY("GameIn",		ch->pcdata->gamein,			o->valuestring);
				STRKEY("GameOut",		ch->pcdata->gameout,		o->valuestring);
				break;
			case 'H':
				if (!str_cmp(key, "HMSP")) {
					get_JSON_short(o, &ch->pcdata->perm_hit, "hit");
					get_JSON_short(o, &ch->pcdata->perm_mana, "mana");
					get_JSON_short(o, &ch->pcdata->perm_stam, "stam");
					fMatch = TRUE; break;
				}

				break;
			case 'I':
				if (!str_cmp(key, "Ignore")) {
					for (cJSON *item = o->child; item != NULL && count < MAX_IGNORE; item = item->next)
						ch->pcdata->ignore[count++] = str_dup(item->valuestring);
					fMatch = TRUE; break;
				}

				STRKEY("Immn",			ch->pcdata->immname,		o->valuestring);
				STRKEY("Immp",			ch->pcdata->immprefix,		o->valuestring);
				break;
			case 'L':
				INTKEY("Lay",			ch->pcdata->lays,			o->valueint);
				INTKEY("Lay_Next",		ch->pcdata->next_lay_countdown,	o->valueint);
				INTKEY("LLev",			ch->pcdata->last_level,		o->valueint);
				INTKEY("LogO",			ch->pcdata->last_logoff,	o->valueint);
				STRKEY("Lsit",			ch->pcdata->last_lsite,		o->valuestring);
				INTKEY("Ltim",			ch->pcdata->last_ltime,		dizzy_scantime(o->valuestring));
				INTKEY("Lsav",			ch->pcdata->last_saved,		dizzy_scantime(o->valuestring));
				break;
			case 'M':
				INTKEY("Mark",			ch->pcdata->mark_room,		o->valueint);
				INTKEY("Mexp",			ch->pcdata->mud_exp,		o->valueint);
				break;
			case 'N':
				if (!str_cmp(key, "Note")) {
					get_JSON_long(o, &ch->pcdata->last_note, "note");
					get_JSON_long(o, &ch->pcdata->last_idea, "idea");
					get_JSON_long(o, &ch->pcdata->last_roleplay, "role");
					get_JSON_long(o, &ch->pcdata->last_immquest, "quest");
					get_JSON_long(o, &ch->pcdata->last_changes, "change");
					get_JSON_long(o, &ch->pcdata->last_personal, "pers");
					get_JSON_long(o, &ch->pcdata->last_trade, "trade");
					fMatch = TRUE; break;
				}

				break;
			case 'P':
				STRKEY("Pass",			ch->pcdata->pwd,		o->valuestring);
				INTKEY("PCkills",		ch->pcdata->pckills,	o->valueint);
				INTKEY("PCkilled",		ch->pcdata->pckilled,	o->valueint);
				INTKEY("PKRank",		ch->pcdata->pkrank,		o->valueint);
				INTKEY("Plyd",			ch->pcdata->played,		o->valueint);
				INTKEY("Plr",			ch->pcdata->plr,		read_flags(o->valuestring));
				INTKEY("Pnts",			ch->pcdata->points,		o->valueint);
				break;
			case 'Q':
				if (!str_cmp(key, "Query")) {
					for (cJSON *item = o->child; item != NULL && count < MAX_QUERY; item = item->next) {
						ch->pcdata->query[count++] = str_dup(item->valuestring);
					}
					fMatch = TRUE; break;
				}

				break;
			case 'R':
				if (!str_cmp(key, "Raff")) {
					for (cJSON *item = o->child; item != NULL && count < MAX_RAFFECT_SLOTS; item = item->next)
						ch->pcdata->raffect[count++] = item->valueint;
					fMatch = TRUE; break;
				}

				STRKEY("Rank",			ch->pcdata->rank,			o->valuestring);
				INTKEY("RmCt",			ch->pcdata->remort_count,	o->valueint);
				INTKEY("RolePnts",		ch->pcdata->rolepoints,		o->valueint);
				break;
			case 'S':
				if (!str_cmp(key, "Sk")) {
					for (cJSON *item = o->child; item != NULL; item = item->next) {
						char *temp = cJSON_GetObjectItem(item, "name")->valuestring;
						int sn = skill_lookup(temp);

						if (sn < 0) {
							fprintf(stderr, "%s", temp);
							bug("Fread_char: unknown skill. ", 0);
							continue;
						}

						ch->pcdata->learned[sn] = cJSON_GetObjectItem(item, "prac")->valueint;
						ch->pcdata->evolution[sn] = cJSON_GetObjectItem(item, "evol")->valueint;
					}
					fMatch = TRUE; break;
				}

				INTKEY("SkillPnts",		ch->pcdata->skillpoints,	o->valueint);
				STRKEY("Stus",			ch->pcdata->status,			o->valuestring);
				STRKEY("Spou",			ch->pcdata->spouse,			o->valuestring);
				INTKEY("SQuestNext",	ch->pcdata->nextsquest,		o->valueint);
				break;
			case 'T':
				if (!str_cmp(key, "THMS")) {
					get_JSON_short(o, &ch->pcdata->trains_to_hit, "hit");
					get_JSON_short(o, &ch->pcdata->trains_to_hit, "mana");
					get_JSON_short(o, &ch->pcdata->trains_to_hit, "stam");
					fMatch = TRUE; break;
				}

				if (!str_cmp(key, "Titl")) {
					set_title(ch, o->valuestring);
					fMatch = TRUE; break;
				}

				INTKEY("TSex",			ch->pcdata->true_sex,		o->valueint);
				break;
			case 'V':
				INTKEY("Video",			ch->pcdata->video,			read_flags(o->valuestring));
				break;
			case 'W':
				STRKEY("Wspr",			ch->pcdata->whisper,		o->valuestring);
				break;
			default:
				// drop down
				break;
		}

		if (!fMatch)
			bugf("fread_player: unknown key %s", key);
	}

	// fix up pc-only stuff here
}

// this could be PC or NPC!  get act flags first
void fread_char(CHAR_DATA *ch, cJSON *json, int version)
{
	if (json == NULL)
		return;

	char buf[MSL];
	sprintf(buf, "Loading %s.", ch->name);
	log_string(buf);

	// unlike old pfiles, the order of calls is important here, because we can't
	// guarantee order within the files. If there are any fields that are depended
	// on by others in the list, load them right here, and use SKIPKEY(key) in the list

	get_JSON_flags(json, &ch->act, "Act");

	// now safe to check IS_NPC

	for (cJSON *o = json->child; o; o = o->next) {
		char *key = o->string;
		bool fMatch = FALSE;
//		int count = 0; // convenience variable to compact this list, resets with every item

		switch (toupper(key[0])) {
			case 'A':
				if (!str_cmp(key, "Affc")) {
					for (cJSON *item = o->child; item != NULL; item = item->next) {
						int sn = skill_lookup(cJSON_GetObjectItem(item, "name")->valuestring);

						if (sn < 0) {
							bug("Fread_char: unknown skill.", 0);
							continue;
						}

						AFFECT_DATA *paf = new_affect();
						paf->type = sn;
						get_JSON_short(item, &paf->where, "where");
						get_JSON_short(item, &paf->level, "level");
						get_JSON_short(item, &paf->duration, "dur");
						get_JSON_short(item, &paf->modifier, "mod");
						get_JSON_short(item, &paf->location, "loc");
						get_JSON_int(item, &paf->bitvector, "bitv");
						get_JSON_short(item, &paf->evolution, "evo");

						if (IS_NPC(ch)) {
							bool found = FALSE;

							/* loop through the pet's spells, only add if they don't have it */
							for (AFFECT_DATA *old_af = ch->affected; old_af; old_af = old_af->next)
								if (old_af->type == paf->type && old_af->location == paf->location) {
									found = TRUE;
									break;
								}

							if (found) {
								free_affect(paf);
								continue;
							}
						}

						paf->next       = ch->affected;
						ch->affected    = paf;
					}
					fMatch = TRUE; break;
				}

				if (!str_cmp(key, "Atrib")) {
					get_JSON_short(o, &ch->perm_stat[STAT_STR], "str");
					get_JSON_short(o, &ch->perm_stat[STAT_INT], "int");
					get_JSON_short(o, &ch->perm_stat[STAT_WIS], "wis");
					get_JSON_short(o, &ch->perm_stat[STAT_DEX], "dex");
					get_JSON_short(o, &ch->perm_stat[STAT_CON], "con");
					get_JSON_short(o, &ch->perm_stat[STAT_CHR], "chr");
					fMatch = TRUE; break;
				}

				// npc only
				if (IS_NPC(ch) && !str_cmp(key, "AtMod")) {
					get_JSON_short(o, &ch->mod_stat[STAT_STR], "str");
					get_JSON_short(o, &ch->mod_stat[STAT_INT], "int");
					get_JSON_short(o, &ch->mod_stat[STAT_WIS], "wis");
					get_JSON_short(o, &ch->mod_stat[STAT_DEX], "dex");
					get_JSON_short(o, &ch->mod_stat[STAT_CON], "con");
					get_JSON_short(o, &ch->mod_stat[STAT_CHR], "chr");
					fMatch = TRUE; break;
				}

				SKIPKEY("Act");
				INTKEY("AfBy",			ch->affected_by,			read_flags(o->valuestring));
				INTKEY("Alig",			ch->alignment,				o->valueint);
				break;
			case 'C':
				INTKEY("Clan",			ch->clan,					clan_lookup(o->valuestring));
				INTKEY("Cla",			ch->class,					o->valueint);
				INTKEY("Comm",			ch->comm,					read_flags(o->valuestring));
				INTKEY("Cnsr",			ch->censor,					read_flags(o->valuestring));
				break;
			case 'D':
				INTKEY("Dam",			ch->damroll,				o->valueint);		// NPC
				STRKEY("Desc",			ch->description,			o->valuestring);
				break;
			case 'E':
				INTKEY("Exp",			ch->exp,					o->valueint);
				break;
			case 'F':
				INTKEY("Fimm",			ch->imm_flags,				read_flags(o->valuestring));
				INTKEY("FRes",			ch->res_flags,				read_flags(o->valuestring));
				INTKEY("FVul",			ch->vuln_flags,				read_flags(o->valuestring));
				break;
			case 'G':
				INTKEY("Gold_in_bank",	ch->gold_in_bank,			o->valueint);
				INTKEY("Gold",			ch->gold,					o->valueint);
				INTKEY("GlDonated",		ch->gold_donated,			o->valueint);
				break;
			case 'H':
				if (!str_cmp(key, "HMS")) {
					get_JSON_short(o, &ch->hit, "hit");
					get_JSON_short(o, &ch->mana, "mana");
					get_JSON_short(o, &ch->stam, "stam");
					fMatch = TRUE; break;
				}

				INTKEY("Hit",			ch->hitroll,				o->valueint); // NPC
				break;
			case 'I':
				INTKEY("Id",			ch->id,						o->valueint);
				INTKEY("Invi",			ch->invis_level,			o->valueint);
				break;
			case 'L':
				INTKEY("Levl",			ch->level,					o->valueint);
				STRKEY("LnD",			ch->long_descr,				o->valuestring);
				INTKEY("Lurk",			ch->lurk_level,				o->valueint);
				break;
			case 'N':
				STRKEY("Name",			ch->name,					o->valuestring);
				break;
			case 'P':
				INTKEY("Pos",			ch->position,				o->valueint);
				INTKEY("Prac",			ch->practice,				o->valueint);
				STRKEY("Prom",			ch->prompt,					o->valuestring);
				break;
			case 'Q':
				INTKEY("QuestPnts",		ch->questpoints,			o->valueint);
				INTKEY("QpDonated",		ch->questpoints_donated,	o->valueint);
				INTKEY("QuestNext",		ch->nextquest,				o->valueint);
				break;
			case 'R':
				INTKEY("Race",			ch->race,					race_lookup(o->valuestring));
				INTKEY("Room",			ch->in_room,				get_room_index(o->valueint));
				INTKEY("Revk",			ch->revoke,					read_flags(o->valuestring));
				break;
			case 'S':
				INTKEY("Save",			ch->saving_throw,			o->valueint); // NPC
				INTKEY("Scro",			ch->lines,					o->valueint);
				INTKEY("Secu",			ch->secure_level,			o->valueint);
				INTKEY("Sex",			ch->sex,					o->valueint); // NPC, reset_char fixes this anyway
				STRKEY("ShD",			ch->short_descr,			o->valuestring);
				INTKEY("Silver_in_bank",ch->silver_in_bank,			o->valueint);
				INTKEY("Silv",			ch->silver,					o->valueint);
				break;
			case 'T':
				INTKEY("Trai",			ch->train,					o->valueint);
				break;
			case 'V':
				SKIPKEY("Vnum"); // for NPCs
				break;
			case 'W':
				INTKEY("Wimp",			ch->wimpy,					o->valueint);
				INTKEY("Wizn",			ch->wiznet,					read_flags(o->valuestring));
				break;
			default:
				// drop down
				break;
		}

		if (!fMatch)
			bugf("fread_char: unknown key %s", key);
	}
}

// read a single item including its contents
OBJ_DATA * fread_obj(cJSON *json, int version) {
	OBJ_DATA *obj = NULL;
	cJSON *o;

	if ((o = cJSON_GetObjectItem(json, "Vnum")) != NULL) {
		OBJ_INDEX_DATA *index = get_obj_index(o->valueint);

		if (index == NULL)
			bug("Fread_obj: bad vnum %d in fread_obj().", o->valueint);
		else {
			obj = create_object(index, -1);

			if (obj == NULL)
				bug("fread_obj: create_object returned NULL", 0);
		}
	}
	else
		bug("fread_obj: no vnum field in JSON object", 0);

//	bug("reading an object", 0);

	if (obj == NULL) { /* either not found or old style */
		bug("obj is null!", 0);
		obj = new_obj();
		obj->name               = str_dup("");
		obj->short_descr        = str_dup("");
		obj->description        = str_dup("");
	}

	for (cJSON *o = json->child; o; o = o->next) {
		char *key = o->string;
		bool fMatch = FALSE;
//		int count = 0; // convenience variable to compact this list, resets with every item

		switch (toupper(key[0])) {
			case 'A':
				if (!str_cmp(key, "Affc")) {
					obj->enchanted = TRUE;

					for (cJSON *item = o->child; item != NULL; item = item->next) {
						int sn = skill_lookup(cJSON_GetObjectItem(item, "name")->valuestring);

						if (sn < 0) {
							bug("Fread_char: unknown skill.", 0);
							continue;
						}

						AFFECT_DATA *paf = new_affect();
						paf->type = sn;

						get_JSON_short(item, &paf->where, "where");
						get_JSON_short(item, &paf->level, "level");
						get_JSON_short(item, &paf->duration, "dur");
						get_JSON_short(item, &paf->modifier, "mod");
						get_JSON_short(item, &paf->location, "loc");
						get_JSON_int(item, &paf->bitvector, "bitv");
						get_JSON_short(item, &paf->evolution, "evo");

						paf->next       = obj->affected;
						obj->affected    = paf;
					}
					fMatch = TRUE; break;
				}
				break;
			case 'C':
				if (!str_cmp(key, "contains")) {
					// this mirrors code for fread_objects, but uses obj_to_obj instead of obj_to_char/locker/strongbox,
					// so the function pointer doesn't work.  maybe find a way to fix and condense?
					for (cJSON *item = o->child; item; item = item->next) {
						OBJ_DATA *content = fread_obj(item, version);

						if (content->pIndexData) {
							if (content->condition == 0)
								content->condition = content->pIndexData->condition;

							obj_to_obj(content, obj);
						}
						else {
							// deal with contents and extract
							while (content->contains) {
								OBJ_DATA *c = content->contains;
								content->contains = c->next_content;
								obj_to_obj(c, obj);
							}

							free_obj(content);
						}
					}
					fMatch = TRUE; break;
				}

				INTKEY("Cond",			obj->condition,				o->valueint);
				INTKEY("Cost",			obj->cost,					o->valueint);
				break;
			case 'D':
				STRKEY("Desc",			obj->description,			o->valuestring);
				break;
			case 'E':
				if (!str_cmp(key, "ExDe")) {
					for (cJSON *item = o->child; item; item = item->next) {
						EXTRA_DESCR_DATA *ed = new_extra_descr();
						ed->keyword             = str_dup(item->string);
						ed->description         = str_dup(item->valuestring);
						ed->next                = obj->extra_descr;
						obj->extra_descr        = ed;
					}
					fMatch = TRUE; break;
				}

//				INTKEY("Enchanted",		obj->enchanted,				o->valueint);
				INTKEY("ExtF",			obj->extra_flags,			o->valueint); // no, not fread_flags
				break;
			case 'I':
				INTKEY("Ityp",			obj->item_type,				o->valueint);
				break;
			case 'L':
				INTKEY("Lev",			obj->level,					o->valueint);
				break;
			case 'M':
				STRKEY("Mat",			obj->material,				o->valuestring);
				break;
			case 'N':
				STRKEY("Name",			obj->name,					o->valuestring);
				break;
			case 'S':
				if (!str_cmp(key, "Splxtra")) {
					int count = 0;
					for (cJSON *item = o->child; item; item = item->next, count++) {
						obj->spell[count] = skill_lookup(cJSON_GetObjectItem(item, "name")->valuestring);
						obj->spell_lev[count] = cJSON_GetObjectItem(item, "level")->valueint;
					}
					fMatch = TRUE; break;
				}

				STRKEY("ShD",			obj->short_descr,			o->valuestring);
				break;
			case 'T':
				INTKEY("Time",			obj->timer,					o->valueint);
				break;
			case 'V':
				if (!str_cmp(key, "Val")) {
					int slot = 0;
					for (cJSON *item = o->child; item; item = item->next, slot++)
						obj->value[slot] = item->valueint;
					fMatch = TRUE; break;
				}

				SKIPKEY("Vnum");
				break;
			case 'W':
				INTKEY("WeaF",			obj->wear_flags,			o->valueint); // no, not read_flags
				INTKEY("Wear",			obj->wear_loc,				o->valueint);
				INTKEY("Wt",			obj->weight,				o->valueint);
				break;
			default:
				break;
		}

		if (!fMatch)
			bugf("fread_obj: unknown key %s", key);
	}

	return obj;
}

// read a list of objects and return the head
void fread_objects(CHAR_DATA *ch, cJSON *contains, void (*obj_to)(OBJ_DATA *, CHAR_DATA *), int version) {
	if (contains == NULL)
		return;

	for (cJSON *item = contains->child; item; item = item->next) {
		OBJ_DATA *content = fread_obj(item, version);

		if (content->pIndexData) {
			if (content->condition == 0)
				content->condition = content->pIndexData->condition;

			(*obj_to)(content, ch);
		}
		else {
			// deal with contents and extract
			while (content->contains) {
				OBJ_DATA *c = content->contains;
				content->contains = c->next_content;
				(*obj_to)(c, ch);
			}

			free_obj(content);
		}
	}
}

/* load a pet from the forgotten reaches */
void fread_pet(CHAR_DATA *ch, cJSON *json, int version)
{
	cJSON *o;

	if (json == NULL)
		return;

	int vnum;

	// error compensation in case their mob goes away, don't poof inventory
	if ((o = cJSON_GetObjectItem(json, "Vnum")) != NULL) {
		vnum = o->valueint;
	}
	else {
		bug("fread_pet: no vnum field in JSON object", 0);
		vnum = MOB_VNUM_FIDO;
	}

	MOB_INDEX_DATA *index = get_mob_index(vnum);

	if (index == NULL) {
		bug("Fread_pet: bad vnum %d in fread_pet().", vnum);
		index = get_mob_index(MOB_VNUM_FIDO);
	}

	CHAR_DATA *pet = create_mobile(index);

	/* Check for memory error. -- Outsider */
	if (!pet) {
		bug("Memory error creating mob in fread_pet().", 0);
		return;
	}

	fread_char(pet, json, version);

	pet->leader = ch;
	pet->master = ch;
	ch->pet = pet;

	/* adjust hp mana stamina up  -- here for speed's sake */
	int percent;
	percent = (current_time - ch->pcdata->last_logoff) * 25 / (2 * 60 * 60);
	percent = UMIN(percent, 100);

	if (percent > 0 && !IS_AFFECTED(ch, AFF_POISON)
	    &&  !IS_AFFECTED(ch, AFF_PLAGUE)) {
		pet->hit    += (pet->max_hit - pet->hit) * percent / 100;
		pet->mana   += (pet->max_mana - pet->mana) * percent / 100;
		pet->stam   += (pet->max_stam - pet->stam) * percent / 100;
	}

	reset_char(pet);
}
