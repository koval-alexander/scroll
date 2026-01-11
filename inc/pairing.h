#ifndef _PAIRING_H_
#define _PAIRING_H_

typedef struct conn_mode {
	struct bt_conn *conn;
	bool in_boot_mode;
} conn_mode_t;

extern struct k_work adv_work;
extern conn_mode_t conn_mode[];
extern volatile bool is_adv_running;

void register_auth_callbacks(void);
void register_pairing_work(void);

void connected(struct bt_conn *conn, uint8_t err);
void disconnected(struct bt_conn *conn, uint8_t reason);

void insert_conn_object(struct bt_conn *conn);
bool is_conn_slot_free(void);
void advertising_start(void);

#endif /* _PAIRING_H_ */