/*
 * Copyright (c) 2013 Cozybit, Inc.
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "mesh_rsn.h"
#include "rsn_supp/wpa.h"
#include "rsn_supp/wpa_ie.h"
#include "ap/wpa_auth.h"
#include "ap/wpa_auth_i.h"
#include "ap/pmksa_cache_auth.h"
#include "crypto/sha256.h"
#include "crypto/random.h"
#include "crypto/aes.h"
#include "crypto/aes_siv.h"
#include "wpas_glue.h"

#define MESH_AUTH_TIMEOUT 10
#define MESH_AUTH_RETRY 3

void mesh_auth_timer(void *eloop_ctx, void *user_data)
{
	struct wpa_supplicant *wpa_s = eloop_ctx;
	struct sta_info *sta = user_data;

	if (sta == NULL)
		return;

	if (sta->sae->state != SAE_ACCEPTED) {
		wpa_printf(MSG_DEBUG, "AUTH: Re-autenticate with "
			   MACSTR " Number of Try (%d) ", MAC2STR(sta->addr),
			   sta->sae_auth_retry);
		if (sta->sae_auth_retry < MESH_AUTH_RETRY) {
			mesh_rsn_auth_sae_sta(wpa_s, sta);
		} else {
			/* If exceed the number of tries, block the STA */
			sta->plink_state = PLINK_BLOCKED;
			sta->sae->state = SAE_NOTHING;
		}
		sta->sae_auth_retry++;
	}
}

static void auth_logger(void *ctx, const u8 *addr, logger_level level,
			const char *txt)
{
	if (addr)
		wpa_printf(MSG_DEBUG, "AUTH: " MACSTR " - %s",
			   MAC2STR(addr), txt);
	else
		wpa_printf(MSG_DEBUG, "AUTH: %s", txt);
}


static const u8 * auth_get_psk(void *ctx, const u8 *addr, const u8 *prev_psk)
{
	struct mesh_rsn *mesh_rsn = ctx;
	struct hostapd_data *hapd = mesh_rsn->wpa_s->ifmsh->bss[0];
	struct sta_info *sta = ap_get_sta(hapd, addr);

	wpa_printf(MSG_DEBUG, "AUTH: %s (addr=" MACSTR " prev_psk=%p)",
		   __func__, MAC2STR(addr), prev_psk);

	if (sta && sta->auth_alg == WLAN_AUTH_SAE) {
		if (!sta->sae || prev_psk)
			return NULL;
		return sta->sae->pmk;
	}
	return NULL;
}

static int auth_set_key(void *ctx, int vlan_id, enum wpa_alg alg,
			const u8 *addr, int idx, u8 *key, size_t key_len)
{
	struct mesh_rsn *mesh_rsn = ctx;
	u8 seq[6];
	int set_tx = 1;

	os_memset(seq, 0, sizeof(seq));

	if (addr) {
		wpa_printf(MSG_DEBUG, "AUTH: %s(alg=%d addr=" MACSTR
			   " key_idx=%d)",
			   __func__, alg, MAC2STR(addr), idx);
	} else {
		wpa_printf(MSG_DEBUG, "AUTH: %s(alg=%d key_idx=%d)",
			   __func__, alg, idx);
	}
	wpa_hexdump_key(MSG_DEBUG, "AUTH: set_key - key", key, key_len);

	if (idx == 4)
		set_tx = 0;

	return wpa_drv_set_key(mesh_rsn->wpa_s, alg, addr, idx,
			       set_tx, seq, 6, key, key_len);
}

static int auth_start_ampe(void *ctx, const u8 *addr)
{
	struct mesh_rsn *mesh_rsn = ctx;
	struct hostapd_data *hapd = mesh_rsn->wpa_s->ifmsh->bss[0];
	struct sta_info *sta = ap_get_sta(hapd, addr);

	if (mesh_rsn->wpa_s->current_ssid->mode != WPAS_MODE_MESH)
		return -1;

	eloop_cancel_timeout(mesh_auth_timer, mesh_rsn->wpa_s, sta);
	mesh_mpm_auth_peer(mesh_rsn->wpa_s, addr);
	return 0;
}

static int
__mesh_rsn_auth_init(struct mesh_rsn *rsn, const u8 *addr)
{
	struct wpa_auth_config conf;
	struct wpa_auth_callbacks cb;
	u8 seq[6] = {};

	wpa_printf(MSG_DEBUG, "AUTH: Initializing group state machine");

	os_memset(&conf, 0, sizeof(conf));
	conf.wpa = 2;
	conf.wpa_key_mgmt = WPA_KEY_MGMT_SAE;
	conf.wpa_pairwise = WPA_CIPHER_CCMP;
	conf.rsn_pairwise = WPA_CIPHER_CCMP;
	conf.wpa_group = WPA_CIPHER_CCMP;
	conf.eapol_version = 0;
	conf.wpa_group_rekey = 600;

	os_memset(&cb, 0, sizeof(cb));
	cb.ctx = rsn;
	cb.logger = auth_logger;
	cb.get_psk = auth_get_psk;
	cb.set_key = auth_set_key;
	cb.start_ampe = auth_start_ampe;

	rsn->auth = wpa_init(addr, &conf, &cb);
	if (rsn->auth == NULL) {
		wpa_printf(MSG_DEBUG, "AUTH: wpa_init() failed");
		return -1;
	}

	/* TODO: support rekeying */
	random_get_bytes(rsn->mgtk, 16);

	/* TODO: don't hardcode key idx */
	/* mcast mgmt */
	wpa_drv_set_key(rsn->wpa_s, WPA_ALG_IGTK, NULL, 4, 1,
			seq, sizeof(seq), rsn->mgtk, sizeof(rsn->mgtk));
	/* mcast data */
	wpa_drv_set_key(rsn->wpa_s, WPA_ALG_CCMP, NULL, 1, 1,
			seq, sizeof(seq), rsn->mgtk, sizeof(rsn->mgtk));

	return 0;
}

static void mesh_rsn_deinit(struct mesh_rsn *rsn)
{
	/* TODO: stuff */
}

struct mesh_rsn *mesh_rsn_auth_init(struct wpa_supplicant *wpa_s,
				    struct mesh_conf *conf)
{
	struct mesh_rsn *mesh_rsn;
	struct hostapd_data *bss = wpa_s->ifmsh->bss[0];

	mesh_rsn = os_zalloc(sizeof(*mesh_rsn));
	if (mesh_rsn == NULL)
		return NULL;
	mesh_rsn->wpa_s = wpa_s;

	if (__mesh_rsn_auth_init(mesh_rsn, wpa_s->own_addr) < 0) {
		mesh_rsn_deinit(mesh_rsn);
		return NULL;
	}

	bss->wpa_auth = mesh_rsn->auth;

	conf->ies = mesh_rsn->auth->wpa_ie;
	conf->ie_len = mesh_rsn->auth->wpa_ie_len;

	wpa_supplicant_rsn_supp_set_config(wpa_s, wpa_s->current_ssid);

	return mesh_rsn;
}

static int index_within_array(const int *array, int idx)
{
	int i;
	for (i = 0; i < idx; i++) {
		if (array[i] == -1)
			return 0;
	}
	return 1;
}

static int mesh_rsn_sae_group(struct wpa_supplicant *wpa_s,
			      struct sae_data *sae)
{
	int *groups = wpa_s->ifmsh->bss[0]->conf->sae_groups;

	/* Configuration may have changed, so validate current index */
	if (!index_within_array(groups, wpa_s->mesh_rsn->sae_group_index))
		return -1;

	for (;;) {
		int group = groups[wpa_s->mesh_rsn->sae_group_index];
		if (group < 0)
			break;
		if (sae_set_group(sae, group) == 0) {
			wpa_dbg(wpa_s, MSG_DEBUG, "SME: Selected SAE group %d",
				sae->group);
		       return 0;
		}
		wpa_s->mesh_rsn->sae_group_index++;
	}

	return -1;
}

struct wpabuf *
mesh_rsn_build_sae_commit(struct wpa_supplicant *wpa_s,
			  struct wpa_ssid *ssid, struct sta_info *sta)
{
	struct wpabuf *buf;
	int len;

	if (ssid->passphrase == NULL) {
		wpa_msg(wpa_s, MSG_DEBUG, "SAE: No password available");
		return NULL;
	}

	if (mesh_rsn_sae_group(wpa_s, sta->sae) < 0) {
		wpa_msg(wpa_s, MSG_DEBUG, "SAE: Failed to select group");
		return NULL;
	}

	if (sae_prepare_commit(wpa_s->own_addr, sta->addr,
			       (u8 *) ssid->passphrase,
			       os_strlen(ssid->passphrase), sta->sae) < 0) {
		wpa_msg(wpa_s, MSG_DEBUG, "SAE: Could not pick PWE");
		return NULL;
	}

	len = wpa_s->mesh_rsn->sae_token ?
		wpabuf_len(wpa_s->mesh_rsn->sae_token) : 0;
	buf = wpabuf_alloc(4 + SAE_COMMIT_MAX_LEN + len);
	if (buf == NULL)
		return NULL;

	sae_write_commit(sta->sae, buf, wpa_s->mesh_rsn->sae_token);

	return buf;
}

static void mesh_rsn_send_auth(struct wpa_supplicant *wpa_s,
			       const u8 *dst, const u8 *src,
			       u16 auth_transaction, u16 resp,
			       struct wpabuf *data)
{
	struct ieee80211_mgmt *auth;
	u8 *buf;
	size_t len;

	len = IEEE80211_HDRLEN + sizeof(auth->u.auth) + wpabuf_len(data);
	buf = os_zalloc(len);
	if (buf == NULL)
		return;

	auth = (struct ieee80211_mgmt *) buf;
	auth->frame_control = IEEE80211_FC(WLAN_FC_TYPE_MGMT,
					   WLAN_FC_STYPE_AUTH);
	os_memcpy(auth->da, dst, ETH_ALEN);
	os_memcpy(auth->sa, src, ETH_ALEN);
	os_memcpy(auth->bssid, dst, ETH_ALEN);

	auth->u.auth.auth_alg = host_to_le16(WLAN_AUTH_SAE);
	auth->u.auth.auth_transaction = host_to_le16(auth_transaction);
	auth->u.auth.status_code = host_to_le16(resp);

	if (data)
		os_memcpy(auth->u.auth.variable,
			  wpabuf_head(data), wpabuf_len(data));

	wpa_msg(wpa_s, MSG_DEBUG, "authentication frame: STA=" MACSTR
		   " auth_transaction=%d resp=%d (IE len=%lu)",
		   MAC2STR(dst), auth_transaction,
		   resp, (unsigned long) wpabuf_len(data));
	if (wpa_drv_send_mlme(wpa_s, buf, len, 0) < 0)
		perror("send_auth_reply: send");

	os_free(buf);
}

/* initiate new SAE authentication with sta */
int mesh_rsn_auth_sae_sta(struct wpa_supplicant *wpa_s,
			  struct sta_info *sta)
{
	struct wpa_ssid *ssid = wpa_s->current_ssid;
	struct wpabuf *buf;

	if (!sta->sae) {
		sta->sae = os_zalloc(sizeof(*sta->sae));
		if (sta->sae == NULL)
			return -1;
		sta->sae->state = SAE_NOTHING;
	}

	buf = mesh_rsn_build_sae_commit(wpa_s, ssid, sta);
	if (!buf)
		return -1;

	sta->sae->state = SAE_COMMITTED;

	wpa_msg(wpa_s, MSG_DEBUG,
		"AUTH: started authentication with SAE peer: "
		MACSTR, MAC2STR(sta->addr));

	wpa_supplicant_set_state(wpa_s, WPA_AUTHENTICATING);

	mesh_rsn_send_auth(wpa_s, sta->addr, wpa_s->own_addr,
			   SAE_COMMITTED, WLAN_STATUS_SUCCESS, buf);

	eloop_register_timeout(MESH_AUTH_TIMEOUT, 0, mesh_auth_timer,
			       wpa_s, sta);

	wpabuf_free(buf);
	return 0;
}

void mesh_rsn_get_pmkid(struct sta_info *sta, u8 *pmkid)
{
	struct wpa_state_machine *sm = sta->wpa_sm;
	/* don't expect wpa auth to cache the pmkid for now */
	rsn_pmkid(sta->sae->pmk, PMK_LEN, sm->wpa_auth->addr,
		  sm->addr, pmkid,
		  wpa_key_mgmt_sha256(sm->wpa_key_mgmt));
}

static void
mesh_rsn_derive_aek(struct mesh_rsn *rsn, struct sta_info *sta)
{
	u8 *myaddr = rsn->auth->addr;
	u8 *peer = sta->addr;
	u8 *addr1 = peer, *addr2 = myaddr;
	u8 context[AES_BLOCK_SIZE];
	/* SAE */
	RSN_SELECTOR_PUT(context, wpa_cipher_to_suite(0, WPA_CIPHER_GCMP));

	if (os_memcmp(myaddr, peer, ETH_ALEN) < 0) {
		addr1 = myaddr;
		addr2 = peer;
	}
	os_memcpy(context + 4, addr1, ETH_ALEN);
	os_memcpy(context + 10, addr2, ETH_ALEN);

	sha256_prf(sta->sae->pmk, sizeof(sta->sae->pmk), "AEK Derivation",
		   context, sizeof(context), sta->aek, sizeof(sta->aek));
}

/* derive mesh temporal key from pmk */
int mesh_rsn_derive_mtk(struct wpa_supplicant *wpa_s, struct sta_info *sta)
{
	u8 *ptr;
	u8 *min, *max;
	u8 smin[2], smax[2];
	size_t nonce_len = sizeof(sta->my_nonce);
	size_t lid_len = sizeof(sta->my_lid);

	/* XXX use rsn pointer? */
	u8 *myaddr = wpa_s->own_addr;
	u8 *peer = sta->addr;

	/* 2 nonces, 2 linkids, akm suite, 2 mac addrs */
	u8 context[64 + 4 + 4 + 12];

	ptr = context;
	if (os_memcmp(sta->my_nonce, sta->peer_nonce, nonce_len) < 0) {
		min = sta->my_nonce;
		max = sta->peer_nonce;
	} else {
		min = sta->peer_nonce;
		max = sta->my_nonce;
	}
	os_memcpy(ptr, min, nonce_len);
	os_memcpy(ptr + nonce_len, max, nonce_len);
	ptr += 2 * nonce_len;

	/* FIXME memcmp here? */
	if (sta->my_lid < sta->peer_lid) {
		smin[0] = (sta->my_lid >> 8) & 0xff;
		smin[1] = sta->my_lid & 0xff;
		smax[0] = (sta->peer_lid >> 8) & 0xff;
		smax[1] = sta->peer_lid & 0xff;
	} else {
		smin[0] = (sta->peer_lid >> 8) & 0xff;
		smin[1] = sta->peer_lid & 0xff;
		smax[0] = (sta->my_lid >> 8) & 0xff;
		smax[1] = sta->my_lid  & 0xff;
	}
	os_memcpy(ptr, smin, lid_len);
	os_memcpy(ptr + lid_len, smax, lid_len);
	ptr += 2 * lid_len;

	/* SAE */
	RSN_SELECTOR_PUT(ptr, wpa_cipher_to_suite(0, WPA_CIPHER_GCMP));
	ptr += 4;

	if (os_memcmp(myaddr, peer, ETH_ALEN) < 0) {
		min = myaddr;
		max = peer;
	} else {
		min = peer;
		max = myaddr;
	}
	os_memcpy(ptr, min, ETH_ALEN);
	os_memcpy(ptr + ETH_ALEN, max, ETH_ALEN);

	sha256_prf(sta->sae->pmk, sizeof(sta->sae->pmk),
		   "Temporal Key Derivation", context, sizeof(context),
		   sta->mtk, sizeof(sta->mtk));
	return 0;
}


void mesh_rsn_init_ampe_sta(struct wpa_supplicant *wpa_s,
			    struct sta_info *sta)
{
	random_get_bytes(sta->my_nonce, 32);
	os_memset(sta->peer_nonce, 0, 32);
	mesh_rsn_derive_aek(wpa_s->mesh_rsn, sta);
	/* TODO: init SIV-AES contexts for AMPE IE encryption */
}

/* insert AMPE and encrypted MIC at @ie.
 * @mesh_rsn: mesh RSN context
 * @sta: STA we're sending to
 * @cat: pointer to category code in frame header.
 * @buf: wpabuf to add encrypted AMPE and MIC to.
 * */
int mesh_rsn_protect_frame(struct mesh_rsn *rsn,
			   struct sta_info *sta, const u8 *cat,
			   struct wpabuf *buf)
{
	struct ieee80211_ampe_ie  *ampe;
	u8 const *ie = wpabuf_head_u8(buf) + wpabuf_len(buf);
	u8 *ampe_ie = NULL, *mic_ie = NULL, *mic_payload;
	const u8 *aad[] = {rsn->auth->addr, sta->addr, cat};
	const size_t aad_len[] = {ETH_ALEN, ETH_ALEN, ie - cat};
	int ret = 0;

	if (AES_BLOCK_SIZE + 2 + sizeof(*ampe) + 2 > wpabuf_tailroom(buf)) {
		wpa_printf(MSG_ERROR, "protect frame: buffer too small");
		return -EINVAL;
	}

	ampe_ie = os_zalloc(2 + sizeof(*ampe));
	if (!ampe_ie) {
		wpa_printf(MSG_ERROR, "protect frame: out of memory");
		return -ENOMEM;
	}

	mic_ie = os_zalloc(2 + AES_BLOCK_SIZE);
	if (!mic_ie) {
		wpa_printf(MSG_ERROR, "protect frame: out of memory");
		ret = -ENOMEM;
		goto free;
	}

	/*  IE: AMPE */
	ampe_ie[0] = WLAN_EID_AMPE;
	ampe_ie[1] = sizeof(*ampe);
	ampe = (struct ieee80211_ampe_ie *) (ampe_ie + 2);

	RSN_SELECTOR_PUT(ampe->selected_pairwise_suite,
		     wpa_cipher_to_suite(WPA_PROTO_RSN, WPA_CIPHER_CCMP));
	os_memcpy(ampe->local_nonce, sta->my_nonce, 32);
	os_memcpy(ampe->peer_nonce, sta->peer_nonce, 32);
	/* incomplete: see 13.5.4 */
	/* TODO: static mgtk for now since we don't support rekeying! */
	os_memcpy(ampe->mgtk, rsn->mgtk, 16);
	/*  TODO: Populate Key RSC */
	os_memset(ampe->key_expiration, 0xff, 4);        /*  expire in 13 decades or so */

	/* IE: MIC */
	mic_ie[0] = WLAN_EID_MIC;
	mic_ie[1] = AES_BLOCK_SIZE;
	wpabuf_put_data(buf, mic_ie, 2);
	/* MIC field is output ciphertext */

	/* encrypt after MIC */
	mic_payload = (u8 *) wpabuf_put(buf, 2 + sizeof(*ampe) + AES_BLOCK_SIZE);
	if (aes_siv_encrypt(sta->aek, ampe_ie, 2 + sizeof(*ampe), 3,
			    aad, aad_len, mic_payload)) {
		wpa_printf(MSG_ERROR, "protect frame: failed to encrypt");
		ret = -ENOMEM;
		goto free;
	}

free:
	if (ampe_ie)
		os_free(ampe_ie);
	if (mic_ie)
		os_free(mic_ie);
	return ret;
}

int mesh_rsn_process_ampe(struct wpa_supplicant *wpa_s,
			  struct sta_info *sta,
			  struct ieee802_11_elems *elems,
			  const u8 *cat,
			  const u8 *start, size_t elems_len)
{
	int ret = 0;
	struct ieee80211_ampe_ie *ampe;
	u8 null_nonce[32] = {};
	u8 ampe_eid;
	u8 ampe_ie_len;
	u8 *ampe_buf = NULL, *crypt = NULL;
	size_t crypt_len;
	const u8 *aad[] = {sta->addr, wpa_s->mesh_rsn->auth->addr, cat};
	const size_t aad_len[] = {ETH_ALEN, ETH_ALEN, (elems->mic - 2) - cat};

	if (!elems->mic || elems->mic_len < AES_BLOCK_SIZE) {
		wpa_msg(wpa_s, MSG_DEBUG, "Mesh RSN: missing mic ie");
		return -1;
	}

	crypt_len = elems_len - (elems->mic - start);
	if (crypt_len < 2) {
		wpa_msg(wpa_s, MSG_DEBUG, "Mesh RSN: missing ampe ie");
		return -1;
	}

	/* crypt is modified by siv_decrypt */
	crypt = os_zalloc(crypt_len);
	if (!crypt) {
		wpa_printf(MSG_ERROR, "Mesh RSN: out of memory");
		ret = -ENOMEM;
		goto free;
	}

	/* output cleartext */
	ampe_buf = os_zalloc(crypt_len);
	if (!ampe_buf) {
		wpa_printf(MSG_ERROR, "Mesh RSN: out of memory");
		ret = -ENOMEM;
		goto free;
	}

	os_memcpy(crypt, elems->mic, crypt_len);

	if (aes_siv_decrypt(sta->aek, crypt, crypt_len, 3,
			    aad, aad_len, ampe_buf)) {
		wpa_printf(MSG_ERROR, "Mesh RSN: frame verification failed!");
		ret = -1;
		goto free;
	}

	ampe_eid = *ampe_buf;
	ampe_ie_len = *(ampe_buf + 1);

	if (ampe_eid != WLAN_EID_AMPE ||
	    ampe_ie_len < sizeof(struct ieee80211_ampe_ie)) {
		wpa_msg(wpa_s, MSG_DEBUG, "Mesh RSN: invalid ampe ie");
		ret = -1;
		goto free;
	}

	ampe = (struct ieee80211_ampe_ie *) (ampe_buf + 2);
	if (os_memcmp(ampe->peer_nonce, null_nonce, 32) != 0 &&
	    os_memcmp(ampe->peer_nonce, sta->my_nonce, 32) != 0) {
		wpa_msg(wpa_s, MSG_DEBUG, "Mesh RSN: invalid peer nonce");
		ret = -1;
		goto free;
	}
	memcpy(sta->peer_nonce, ampe->local_nonce, sizeof(ampe->local_nonce));
	memcpy(sta->mgtk, ampe->mgtk, sizeof(ampe->mgtk));

	/* todo parse mgtk expiration */
free:
	if (crypt)
		os_free(crypt);
	if (ampe_buf)
		os_free(ampe_buf);
	return ret;
}
