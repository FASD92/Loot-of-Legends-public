CREATE TABLE player_account (
    account_id BIGINT NOT NULL AUTO_INCREMENT,
    nickname VARCHAR(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NULL,
    created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    PRIMARY KEY (account_id),
    UNIQUE KEY uk_player_account_nickname (nickname)
);

CREATE TABLE player_oauth_identity (
    provider VARCHAR(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
    provider_subject VARCHAR(191) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
    account_id BIGINT NOT NULL,
    created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    PRIMARY KEY (provider, provider_subject),
    CONSTRAINT fk_player_oauth_identity_account
        FOREIGN KEY (account_id) REFERENCES player_account (account_id)
);
